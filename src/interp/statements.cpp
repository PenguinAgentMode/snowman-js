// Evaluation of statements and declarations (ECMA-262 14), with their completion values.
#include "interp/dispose.h"
#include "interp/interpreter.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/vm.h"

namespace snowman::interp {

using namespace ast;

namespace {

// Restores the running context's LexicalEnvironment on scope exit.
class LexicalEnvironmentScope {
 public:
  LexicalEnvironmentScope() : context_(runningContext()), saved_(context_->lexicalEnvironment) {}
  ~LexicalEnvironmentScope() { context_->lexicalEnvironment = saved_; }
  Environment* saved() const { return saved_; }
  LexicalEnvironmentScope(const LexicalEnvironmentScope&) = delete;
  LexicalEnvironmentScope& operator=(const LexicalEnvironmentScope&) = delete;

 private:
  InterpContext* context_;
  Environment* saved_;
};

// LoopContinues(completion, labelSet)
bool LoopContinues(const Completion& completion, const LabelSet* labels) {
  if (completion.type == CompletionType::Normal) return true;
  if (completion.type != CompletionType::Continue) return false;
  if (!completion.target) return true;
  if (labels) {
    for (const std::u16string* label : *labels) {
      if (*label == *completion.target) return true;
    }
  }
  return false;
}

// The result of a breakable statement under LabelledEvaluation: an unlabelled break ends it normally.
Completion breakableResult(Completion completion) {
  if (completion.type == CompletionType::Break && !completion.target) {
    return Completion::normal(completion.value.isEmpty() ? Value::undefined() : completion.value);
  }
  return completion;
}

void setLexicalEnvironment(Environment* env) { runningContext()->lexicalEnvironment = env; }

// ---- Declarations ----

Result<void> evaluateVariableDeclaration(const VariableDeclaration* node) {
  bool isVar = node->kind == VariableKind::Var;
  bool isUsing = node->kind == VariableKind::Using || node->kind == VariableKind::AwaitUsing;
  for (const VariableDeclarator* declarator : node->declarations) {
    const Node* id = declarator->id;
    if (id->type == NodeType::Identifier) {
      String* name = identifierName(id->as<Identifier>());
      if (isVar && !declarator->init) continue;
      Reference lhs = TRY(ResolveBinding(name));
      Value value = Value::undefined();
      if (declarator->init) value = TRY(evaluateNamed(declarator->init, Value(name)));
      if (isVar) {
        TRY(PutValue(lhs, value));
      } else {
        // InitializeReferencedBinding(lhs, value, hint): the resource is added before the binding is set.
        if (isUsing) TRY(addUsingResource(lhs.env, value, node->kind));
        TRY(InitializeReferencedBinding(lhs, value));
      }
      continue;
    }
    Value rval = TRY(evaluate(declarator->init));
    TRY(BindingInitialization(id, rval, isVar ? nullptr : runningContext()->lexicalEnvironment));
  }
  return {};
}

Result<Completion> evaluateFunctionDeclarationStatement(const FunctionDeclaration* node) {
  if (node->interpCache) {
    // Annex B.3.2.1 / B.3.2.2 / B.3.2.3: copy the block binding to the var scope.
    InterpContext* context = runningContext();
    String* name = identifierName(node->id);
    Value fobj = TRY(context->lexicalEnvironment->GetBindingValue(name, false));
    TRY(context->variableEnvironment->SetMutableBinding(name, fobj, false));
  }
  return Completion::normal();
}

Result<Completion> evaluateClassDeclaration(const ClassDeclaration* node) {
  String* className = node->id ? identifierName(node->id) : atom(u"default");
  Value value = TRY(ClassDefinitionEvaluation(node, className, Value(className)));
  String* bindingName = node->id ? className : atom(u"*default*");
  TRY(runningContext()->lexicalEnvironment->InitializeBinding(bindingName, value));
  return Completion::normal();
}

// ---- Blocks ----

Result<Completion> evaluateScopedStatements(const std::vector<Node*>& statements,
                                            const std::vector<LexicalDeclaration>& declarations) {
  if (declarations.empty()) return evaluateStatementList(statements);
  LexicalEnvironmentScope scope;
  auto* blockEnv = snowman::make<DeclarativeEnvironment>(scope.saved());
  BlockDeclarationInstantiation(declarations, blockEnv);
  setLexicalEnvironment(blockEnv);
  return DisposeResources(blockEnv, evaluateStatementList(statements));
}

// ---- if / with / switch ----

Result<Completion> evaluateClause(const Node* statement, const BlockScope& scope) {
  if (scope.lexicallyScopedDeclarations.empty()) return evaluateStatement(statement);
  // B.3.3: `if (x) function f() {}` behaves as if the function were in a block of its own.
  LexicalEnvironmentScope envScope;
  auto* blockEnv = snowman::make<DeclarativeEnvironment>(envScope.saved());
  BlockDeclarationInstantiation(scope.lexicallyScopedDeclarations, blockEnv);
  setLexicalEnvironment(blockEnv);
  return evaluateStatement(statement);
}

Result<Completion> evaluateIf(const IfStatement* node) {
  bool exprValue = ToBoolean(TRY(evaluate(node->test)));
  Completion completion;
  if (exprValue) {
    completion = TRY(evaluateClause(node->consequent, node->consequentScope));
  } else if (node->alternate) {
    completion = TRY(evaluateClause(node->alternate, node->alternateScope));
  } else {
    return Completion::normal(Value::undefined());
  }
  return UpdateEmpty(completion, Value::undefined());
}

Result<Completion> evaluateWith(const WithStatement* node) {
  Value value = TRY(evaluate(node->object));
  Object* obj = TRY(ToObject(value));
  LexicalEnvironmentScope scope;
  setLexicalEnvironment(snowman::make<ObjectEnvironment>(obj, true, scope.saved()));
  Completion completion = TRY(evaluateStatement(node->body));
  return UpdateEmpty(completion, Value::undefined());
}

Result<Completion> evaluateCaseClause(const SwitchCase* clause) { return evaluateStatementList(clause->consequent); }

Result<bool> CaseClauseIsSelected(const SwitchCase* clause, Value input) {
  Value clauseSelector = TRY(evaluate(clause->test));
  return IsStrictlyEqual(input, clauseSelector);
}

Result<Completion> CaseBlockEvaluation(const SwitchStatement* node, Value input) {
  Value V = Value::undefined();
  const auto& cases = node->cases;
  size_t defaultIndex = cases.size();
  for (size_t i = 0; i < cases.size(); i++) {
    if (!cases[i]->test) defaultIndex = i;
  }
  auto run = [&](size_t i) -> Result<std::optional<Completion>> {
    Completion R = TRY(evaluateCaseClause(cases[i]));
    if (!R.value.isEmpty()) V = R.value;
    if (R.isAbrupt()) return std::optional<Completion>(UpdateEmpty(R, V));
    return std::optional<Completion>();
  };
  bool found = false;
  for (size_t i = 0; i < defaultIndex; i++) {
    if (!found) found = TRY(CaseClauseIsSelected(cases[i], input));
    if (found) {
      auto abrupt = TRY(run(i));
      if (abrupt) return *abrupt;
    }
  }
  if (defaultIndex == cases.size()) return Completion::normal(V);
  bool foundInB = false;
  if (!found) {
    for (size_t i = defaultIndex + 1; i < cases.size(); i++) {
      if (!foundInB) foundInB = TRY(CaseClauseIsSelected(cases[i], input));
      if (foundInB) {
        auto abrupt = TRY(run(i));
        if (abrupt) return *abrupt;
      }
    }
  }
  if (foundInB) return Completion::normal(V);
  for (size_t i = defaultIndex; i < cases.size(); i++) {
    auto abrupt = TRY(run(i));
    if (abrupt) return *abrupt;
  }
  return Completion::normal(V);
}

Result<Completion> evaluateSwitch(const SwitchStatement* node) {
  Value switchValue = TRY(evaluate(node->discriminant));
  LexicalEnvironmentScope scope;
  if (node->lexicallyScopedDeclarations.empty()) return CaseBlockEvaluation(node, switchValue);
  auto* blockEnv = snowman::make<DeclarativeEnvironment>(scope.saved());
  BlockDeclarationInstantiation(node->lexicallyScopedDeclarations, blockEnv);
  setLexicalEnvironment(blockEnv);
  return DisposeResources(blockEnv, CaseBlockEvaluation(node, switchValue));
}

// ---- try ----

Result<Completion> CatchClauseEvaluation(const CatchClause* handler, Value thrownValue) {
  if (!handler->param) return evaluateStatement(handler->body);
  LexicalEnvironmentScope scope;
  auto* catchEnv = snowman::make<DeclarativeEnvironment>(scope.saved());
  catchEnv->isCatchEnvironment = true;
  for (const auto& argName : handler->paramNames) TRY(catchEnv->CreateMutableBinding(atomFor(argName), false));
  setLexicalEnvironment(catchEnv);
  TRY(BindingInitialization(handler->param, thrownValue, catchEnv));
  return evaluateStatement(handler->body);
}

Result<Completion> evaluateTry(const TryStatement* node) {
  Result<Completion> C = evaluateStatement(node->block);
  if (C.isThrow() && node->handler && !isReturnMarker(C.thrown())) {
    C = CatchClauseEvaluation(node->handler, C.thrown());
  }
  if (node->finalizer) {
    Completion F = TRY(evaluateStatement(node->finalizer));
    if (F.isAbrupt()) return UpdateEmpty(F, Value::undefined());
  }
  if (C.isThrow()) return Throw{C.thrown()};
  return UpdateEmpty(C.value(), Value::undefined());
}

// ---- Loops ----

Result<Completion> evaluateDoWhile(const DoWhileStatement* node, const LabelSet* labels) {
  Value V = Value::undefined();
  while (true) {
    Completion stmtResult = TRY(evaluateStatement(node->body));
    if (!LoopContinues(stmtResult, labels)) return UpdateEmpty(stmtResult, V);
    if (!stmtResult.value.isEmpty()) V = stmtResult.value;
    if (!ToBoolean(TRY(evaluate(node->test)))) return Completion::normal(V);
    heap().maybeCollect();
  }
}

Result<Completion> evaluateWhile(const WhileStatement* node, const LabelSet* labels) {
  Value V = Value::undefined();
  while (true) {
    if (!ToBoolean(TRY(evaluate(node->test)))) return Completion::normal(V);
    Completion stmtResult = TRY(evaluateStatement(node->body));
    if (!LoopContinues(stmtResult, labels)) return UpdateEmpty(stmtResult, V);
    if (!stmtResult.value.isEmpty()) V = stmtResult.value;
    heap().maybeCollect();
  }
}

// CreatePerIterationEnvironment(perIterationBindings)
Result<void> CreatePerIterationEnvironment(const std::vector<String*>& bindings) {
  if (bindings.empty()) return {};
  InterpContext* context = runningContext();
  Environment* lastIterationEnv = context->lexicalEnvironment;
  auto* thisIterationEnv = snowman::make<DeclarativeEnvironment>(lastIterationEnv->outer);
  for (String* bn : bindings) {
    TRY(thisIterationEnv->CreateMutableBinding(bn, false));
    Value lastValue = TRY(lastIterationEnv->GetBindingValue(bn, true));
    TRY(thisIterationEnv->InitializeBinding(bn, lastValue));
  }
  context->lexicalEnvironment = thisIterationEnv;
  return {};
}

Result<Completion> ForBodyEvaluation(const ForStatement* node, const std::vector<String*>& perIterationBindings,
                                     const LabelSet* labels) {
  Value V = Value::undefined();
  TRY(CreatePerIterationEnvironment(perIterationBindings));
  while (true) {
    if (node->test) {
      if (!ToBoolean(TRY(evaluate(node->test)))) return Completion::normal(V);
    }
    Completion result = TRY(evaluateStatement(node->body));
    if (!LoopContinues(result, labels)) return UpdateEmpty(result, V);
    if (!result.value.isEmpty()) V = result.value;
    TRY(CreatePerIterationEnvironment(perIterationBindings));
    if (node->update) TRY(evaluate(node->update));
    heap().maybeCollect();
  }
}

Result<Completion> evaluateFor(const ForStatement* node, const LabelSet* labels) {
  const Node* init = node->init;
  if (init && init->type == NodeType::VariableDeclaration && init->as<VariableDeclaration>()->kind != VariableKind::Var) {
    auto* declaration = init->as<VariableDeclaration>();
    LexicalEnvironmentScope scope;
    auto* loopEnv = snowman::make<DeclarativeEnvironment>(scope.saved());
    bool isConst = declaration->kind != VariableKind::Let;
    std::vector<String*> boundNames;
    for (const auto& d : node->lexicallyScopedDeclarations) {
      for (const auto& dn : d.boundNames) {
        String* name = atomFor(dn);
        if (isConst) {
          TRY(loopEnv->CreateImmutableBinding(name, true));
        } else {
          TRY(loopEnv->CreateMutableBinding(name, false));
        }
        boundNames.push_back(name);
      }
    }
    setLexicalEnvironment(loopEnv);
    Result<void> forDcl = evaluateVariableDeclaration(declaration);
    if (forDcl.isThrow()) return DisposeResources(loopEnv, Result<Completion>(Throw{forDcl.thrown()}));
    // Per-iteration copies are unobservable when nothing in the loop can capture an iteration's environment.
    if (isConst || !loopMayCaptureEnvironment(node)) boundNames.clear();
    return DisposeResources(loopEnv, ForBodyEvaluation(node, boundNames, labels));
  }
  if (init) {
    if (init->type == NodeType::VariableDeclaration) {
      TRY(evaluateVariableDeclaration(init->as<VariableDeclaration>()));
    } else {
      TRY(evaluate(init));
    }
  }
  return ForBodyEvaluation(node, {}, labels);
}

enum class IterationKind { Enumerate, Iterate, AsyncIterate };
enum class LhsKind { Assignment, VarBinding, LexicalBinding };

// The for-in iterator of 14.7.5.10, driven directly.
class ForInIterator {
 public:
  explicit ForInIterator(Object* object) : object_(object) {}

  // The next key, or nullptr when done.
  Result<String*> next() {
    while (true) {
      if (!object_) return static_cast<String*>(nullptr);
      if (!visited_) {
        std::vector<PropertyKey> keys = TRY(object_->OwnPropertyKeys());
        remaining_.clear();
        for (const PropertyKey& key : keys) {
          if (!key.isSymbol()) remaining_.push_back(key.toValue());
        }
        position_ = 0;
        visited_ = true;
      }
      while (position_ < remaining_.size()) {
        Value r = remaining_[position_++];
        bool seen = false;
        for (const Value& v : visitedKeys_) {
          if (equals(v.asString(), r.asString())) {
            seen = true;
            break;
          }
        }
        if (seen) continue;
        PropertyKey key(r.asString());
        auto desc = TRY(object_->GetOwnProperty(key));
        if (!desc) continue;
        visitedKeys_.push_back(r);
        if (desc->enumerable.value_or(false)) return r.asString();
      }
      object_ = TRY(object_->GetPrototypeOf());
      visited_ = false;
    }
  }

 private:
  Object* object_;
  bool visited_ = false;
  ValueVector remaining_;
  size_t position_ = 0;
  ValueVector visitedKeys_;
};

bool isDestructuring(const Node* node) {
  return node->type == NodeType::ObjectPattern || node->type == NodeType::ArrayPattern;
}

// The completion IteratorClose sees for a loop body's abrupt completion (the return marker is a return).
Result<void> closeCompletionFor(const Result<void>& status) {
  if (status.isThrow() && !isReturnMarker(status.thrown())) return Throw{status.thrown()};
  return {};
}

template <class T>
Result<T> closeIterator(const IteratorRecord& record, IterationKind kind, Result<T> completion) {
  Result<void> status;
  if (completion.isThrow()) status = Throw{completion.thrown()};
  Result<void> closed = kind == IterationKind::AsyncIterate ? AsyncIteratorClose(record, closeCompletionFor(status))
                                                            : IteratorClose(record, closeCompletionFor(status));
  if (completion.isThrow()) {
    if (isReturnMarker(completion.thrown()) && closed.isThrow()) return Throw{closed.thrown()};
    return Throw{completion.thrown()};
  }
  if (closed.isThrow()) return Throw{closed.thrown()};
  return completion;
}

Result<Completion> evaluateForInOf(const Node* node, const Node* left, const Node* right, const Node* body,
                                   const std::vector<LexicalDeclaration>& declarations, IterationKind kind,
                                   const LabelSet* labels) {
  InterpContext* context = runningContext();
  Environment* oldEnv = context->lexicalEnvironment;

  LhsKind lhsKind = LhsKind::Assignment;
  const Node* lhs = left;
  bool isConst = false;
  VariableKind declarationKind = VariableKind::Var;
  if (left->type == NodeType::VariableDeclaration) {
    auto* declaration = left->as<VariableDeclaration>();
    lhs = declaration->declarations[0]->id;
    declarationKind = declaration->kind;
    if (declaration->kind == VariableKind::Var) {
      lhsKind = LhsKind::VarBinding;
      // B.3.5: for (var x = init in obj)
      if (const Node* init = declaration->declarations[0]->init) {
        String* name = identifierName(lhs->as<Identifier>());
        Reference ref = TRY(ResolveBinding(name));
        Value value = TRY(evaluateNamed(init, Value(name)));
        TRY(PutValue(ref, value));
      }
    } else {
      lhsKind = LhsKind::LexicalBinding;
      isConst = declaration->kind != VariableKind::Let;
    }
  }

  // ForIn/OfHeadEvaluation
  Value exprValue;
  if (lhsKind == LhsKind::LexicalBinding && !declarations.empty()) {
    auto* newEnv = snowman::make<DeclarativeEnvironment>(oldEnv);
    for (const auto& d : declarations) {
      for (const auto& dn : d.boundNames) TRY(newEnv->CreateMutableBinding(atomFor(dn), false));
    }
    context->lexicalEnvironment = newEnv;
    Result<Value> result = evaluate(right);
    context->lexicalEnvironment = oldEnv;
    exprValue = TRY(std::move(result));
  } else {
    exprValue = TRY(evaluate(right));
  }

  std::optional<ForInIterator> forIn;
  IteratorRecord iteratorRecord;
  if (kind == IterationKind::Enumerate) {
    if (exprValue.isNullish()) return Completion{CompletionType::Break, Value::empty(), nullptr};
    forIn.emplace(TRY(ToObject(exprValue)));
  } else if (kind == IterationKind::AsyncIterate) {
    iteratorRecord = TRY(GetAsyncIterator(exprValue));
  } else {
    iteratorRecord = TRY(GetIterator(exprValue, IteratorKind::Sync));
  }
  Rooted<Object> iteratorRoot(iteratorRecord.iterator);
  ValueVector nextMethodRoot{iteratorRecord.nextMethod};

  // ForIn/OfBodyEvaluation
  Value V = Value::undefined();
  DeclarativeEnvironment* reusableEnv = nullptr;
  bool destructuring = isDestructuring(lhs);
  while (true) {
    Value nextValue;
    if (forIn) {
      String* key = TRY(forIn->next());
      if (!key) return Completion::normal(V);
      nextValue = Value(key);
    } else {
      Value nextResult = TRY(Call(iteratorRecord.nextMethod, Value(iteratorRecord.iterator)));
      if (kind == IterationKind::AsyncIterate) nextResult = TRY(Await(nextResult));
      if (!nextResult.isObject()) return ThrowTypeError("Iterator result is not an object");
      bool done = TRY(IteratorComplete(nextResult.asObject()));
      if (done) return Completion::normal(V);
      nextValue = TRY(IteratorValue(nextResult.asObject()));
    }

    Result<void> status;
    DeclarativeEnvironment* iterationEnv = nullptr;
    if (lhsKind == LhsKind::LexicalBinding && reusableEnv) {
      // Nothing can capture an iteration's environment: reuse one, back in its TDZ state.
      iterationEnv = reusableEnv;
      iterationEnv->resetBindings();
      context->lexicalEnvironment = iterationEnv;
    } else if (lhsKind == LhsKind::LexicalBinding) {
      iterationEnv = snowman::make<DeclarativeEnvironment>(oldEnv);
      if (!loopMayCaptureEnvironment(node)) reusableEnv = iterationEnv;
      for (const auto& d : declarations) {
        for (const auto& dn : d.boundNames) {
          if (isConst) {
            (void)iterationEnv->CreateImmutableBinding(atomFor(dn), true);
          } else {
            (void)iterationEnv->CreateMutableBinding(atomFor(dn), false);
          }
        }
      }
      context->lexicalEnvironment = iterationEnv;
    }
    if (!destructuring) {
      Result<Reference> lhsRef = lhsKind == LhsKind::LexicalBinding
                                     ? ResolveBinding(identifierName(lhs->as<Identifier>()))
                                     : evaluateReference(lhs);
      if (lhsRef.isThrow()) {
        status = Throw{lhsRef.thrown()};
      } else if (lhsKind == LhsKind::LexicalBinding) {
        if (declarationKind == VariableKind::Using || declarationKind == VariableKind::AwaitUsing) {
          status = addUsingResource(iterationEnv, nextValue, declarationKind);
        }
        if (!status.isThrow()) status = InitializeReferencedBinding(lhsRef.value(), nextValue);
      } else {
        status = PutValue(lhsRef.value(), nextValue);
      }
    } else if (lhsKind == LhsKind::Assignment) {
      status = DestructuringAssignmentEvaluation(lhs, nextValue);
    } else if (lhsKind == LhsKind::VarBinding) {
      status = BindingInitialization(lhs, nextValue, nullptr);
    } else {
      status = BindingInitialization(lhs, nextValue, iterationEnv);
    }
    if (status.isThrow()) {
      context->lexicalEnvironment = oldEnv;
      if (kind == IterationKind::Enumerate) return Throw{status.thrown()};
      Result<Completion> abrupt = Throw{status.thrown()};
      return closeIterator(iteratorRecord, kind, abrupt);
    }

    Result<Completion> result = evaluateStatement(body);
    if (iterationEnv) result = DisposeResources(iterationEnv, std::move(result));
    context->lexicalEnvironment = oldEnv;
    if (result.isThrow() || !LoopContinues(result.value(), labels)) {
      if (kind == IterationKind::Enumerate) {
        if (result.isThrow()) return result;
        return UpdateEmpty(result.value(), V);
      }
      if (result.isNormal()) result = UpdateEmpty(result.value(), V);
      return closeIterator(iteratorRecord, kind, std::move(result));
    }
    if (!result.value().value.isEmpty()) V = result.value().value;
    heap().maybeCollect();
  }
}

Result<Completion> evaluateLabelledItem(const Node* node, const LabelSet* labels);

Result<Completion> evaluateLabelled(const LabeledStatement* node, const LabelSet* labels) {
  const std::u16string* label = &node->label->name;
  LabelSet newLabelSet = labels ? *labels : LabelSet();
  newLabelSet.push_back(label);
  Completion stmtResult = TRY(evaluateLabelledItem(node->body, &newLabelSet));
  if (stmtResult.type == CompletionType::Break && stmtResult.target && *stmtResult.target == *label) {
    return Completion::normal(stmtResult.value);
  }
  return stmtResult;
}

// LabelledEvaluation of a statement with a label set.
Result<Completion> evaluateLabelledItem(const Node* node, const LabelSet* labels) {
  switch (node->type) {
    case NodeType::LabeledStatement:
      return evaluateLabelled(node->as<LabeledStatement>(), labels);
    case NodeType::DoWhileStatement:
      return breakableResult(TRY(evaluateDoWhile(node->as<DoWhileStatement>(), labels)));
    case NodeType::WhileStatement:
      return breakableResult(TRY(evaluateWhile(node->as<WhileStatement>(), labels)));
    case NodeType::ForStatement:
      return breakableResult(TRY(evaluateFor(node->as<ForStatement>(), labels)));
    case NodeType::ForInStatement: {
      auto* forIn = node->as<ForInStatement>();
      return breakableResult(TRY(evaluateForInOf(node, forIn->left, forIn->right, forIn->body,
                                                 forIn->lexicallyScopedDeclarations, IterationKind::Enumerate, labels)));
    }
    case NodeType::ForOfStatement: {
      auto* forOf = node->as<ForOfStatement>();
      return breakableResult(TRY(evaluateForInOf(node, forOf->left, forOf->right, forOf->body,
                                                 forOf->lexicallyScopedDeclarations,
                                                 forOf->await ? IterationKind::AsyncIterate : IterationKind::Iterate,
                                                 labels)));
    }
    case NodeType::SwitchStatement:
      return breakableResult(TRY(evaluateSwitch(node->as<SwitchStatement>())));
    default:
      return evaluateStatement(node, nullptr);
  }
}

}  // namespace

Result<Completion> evaluateStatementList(const std::vector<Node*>& statements) {
  Value value = Value::empty();
  for (const Node* statement : statements) {
    Completion completion = TRY(evaluateStatement(statement));
    if (completion.isAbrupt()) return UpdateEmpty(completion, value);
    if (!completion.value.isEmpty()) value = completion.value;
  }
  return Completion::normal(value);
}

Result<Completion> evaluateBlock(const BlockStatement* block) {
  return evaluateScopedStatements(block->body, block->lexicallyScopedDeclarations);
}

Result<Completion> evaluateStatement(const Node* node, const LabelSet* labels) {
  TRY(CheckStack());
  switch (node->type) {
    case NodeType::ExpressionStatement:
      return Completion::normal(TRY(evaluate(node->as<ExpressionStatement>()->expression)));
    case NodeType::VariableDeclaration:
      TRY(evaluateVariableDeclaration(node->as<VariableDeclaration>()));
      return Completion::normal();
    case NodeType::FunctionDeclaration:
      return evaluateFunctionDeclarationStatement(node->as<FunctionDeclaration>());
    case NodeType::ClassDeclaration:
      return evaluateClassDeclaration(node->as<ClassDeclaration>());
    case NodeType::BlockStatement:
      return evaluateBlock(node->as<BlockStatement>());
    case NodeType::EmptyStatement:
    case NodeType::DebuggerStatement:
      return Completion::normal();
    case NodeType::IfStatement:
      return evaluateIf(node->as<IfStatement>());
    case NodeType::WithStatement:
      return evaluateWith(node->as<WithStatement>());
    case NodeType::ReturnStatement: {
      auto* ret = node->as<ReturnStatement>();
      if (!ret->argument) return Completion{CompletionType::Return, Value::undefined(), nullptr};
      Value exprValue = TRY(evaluate(ret->argument));
      if (inAsyncGenerator()) exprValue = TRY(Await(exprValue));
      return Completion{CompletionType::Return, exprValue, nullptr};
    }
    case NodeType::BreakStatement: {
      auto* brk = node->as<BreakStatement>();
      return Completion{CompletionType::Break, Value::empty(), brk->label ? &brk->label->name : nullptr};
    }
    case NodeType::ContinueStatement: {
      auto* cont = node->as<ContinueStatement>();
      return Completion{CompletionType::Continue, Value::empty(), cont->label ? &cont->label->name : nullptr};
    }
    case NodeType::ThrowStatement:
      return Throw{TRY(evaluate(node->as<ThrowStatement>()->argument))};
    case NodeType::TryStatement:
      return evaluateTry(node->as<TryStatement>());
    case NodeType::LabeledStatement:
    case NodeType::DoWhileStatement:
    case NodeType::WhileStatement:
    case NodeType::ForStatement:
    case NodeType::ForInStatement:
    case NodeType::ForOfStatement:
    case NodeType::SwitchStatement:
      return evaluateLabelledItem(node, labels);
    case NodeType::ImportDeclaration:
      return Completion::normal();
    case NodeType::ExportNamedDeclaration: {
      auto* exportNode = node->as<ExportNamedDeclaration>();
      if (!exportNode->declaration) return Completion::normal();
      return evaluateStatement(exportNode->declaration);
    }
    case NodeType::ExportDefaultDeclaration: {
      auto* exportNode = node->as<ExportDefaultDeclaration>();
      const Node* declaration = exportNode->declaration;
      if (declaration->type == NodeType::FunctionDeclaration) return Completion::normal();
      if (declaration->type == NodeType::ClassDeclaration) return evaluateStatement(declaration);
      Value value = TRY(evaluateNamed(declaration, Value(atom(u"default"))));
      TRY(runningContext()->lexicalEnvironment->InitializeBinding(atom(u"*default*"), value));
      return Completion::normal();
    }
    case NodeType::ExportAllDeclaration:
      return Completion::normal();
    default:
      return Completion::normal(TRY(evaluate(node)));
  }
}

}  // namespace snowman::interp
