// ECMAScript function objects (10.2): [[Call]], [[Construct]], creation, and FunctionDeclarationInstantiation.
#include "interp/function.h"

#include "interp/dispose.h"
#include "interp/interpreter.h"
#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/vm.h"

namespace snowman::interp {

namespace {

const std::vector<ast::Node*> kNoParams;

const ast::VarScope* varScopeOf(const ECMAScriptFunction* F) {
  if (F->codeKind == CodeKind::Function) return F->function;
  if (F->codeKind == CodeKind::StaticBlock) return static_cast<const ast::StaticBlock*>(F->code);
  return nullptr;
}

const std::vector<ast::Node*>& formalsOf(const ECMAScriptFunction* F) {
  return F->codeKind == CodeKind::Function ? F->function->params : kNoParams;
}

// PrepareForOrdinaryCall: fills in the callee context (the caller pushes it).
void PrepareForOrdinaryCall(ECMAScriptFunction* F, Value newTarget, InterpContext& calleeContext) {
  calleeContext.function = F;
  calleeContext.realm = F->realm();
  calleeContext.scriptOrModule = F->scriptOrModule;
  calleeContext.strict = F->strict;
  auto* localEnv = snowman::make<FunctionEnvironment>(F, newTarget, F->environment, F->thisMode == ThisMode::Lexical);
  calleeContext.lexicalEnvironment = localEnv;
  calleeContext.variableEnvironment = localEnv;
  calleeContext.privateEnvironment = F->privateEnvironment;
}

// OrdinaryCallBindThis
void OrdinaryCallBindThis(ECMAScriptFunction* F, InterpContext& calleeContext, Value thisArgument) {
  if (F->thisMode == ThisMode::Lexical) return;
  Value thisValue;
  if (F->thisMode == ThisMode::Strict) {
    thisValue = thisArgument;
  } else if (thisArgument.isNullish()) {
    thisValue = globalEnvironment(F->realm())->globalThisValue;
  } else {
    thisValue = ToObject(thisArgument).release();
  }
  auto* localEnv = static_cast<FunctionEnvironment*>(calleeContext.lexicalEnvironment);
  (void)localEnv->BindThisValue(thisValue);
}

// The default class constructor (ClassDefinitionEvaluation step 14.a), a built-in function's behaviour.
Result<Object*> defaultConstructorBehaviour(ECMAScriptFunction* F, Args args, Object* newTarget) {
  Object* result;
  if (F->derived) {
    Object* func = F->prototype();
    if (!IsConstructor(func ? Value(func) : Value::null())) {
      return ThrowTypeError("Super constructor is not a constructor");
    }
    result = TRY(Construct(func, args, newTarget));
  } else {
    result = TRY(OrdinaryCreateFromConstructor<Object>(newTarget, Intrinsic::ObjectPrototype));
  }
  TRY(InitializeInstanceElements(result, F));
  return result;
}

}  // namespace

Object* homeObjectOf(Object* function) {
  auto* F = function ? function->as<ECMAScriptFunction>() : nullptr;
  return F ? F->homeObject : nullptr;
}

void ECMAScriptFunction::trace(Tracer& tracer) {
  FunctionObject::trace(tracer);
  tracer.visit(environment);
  tracer.visit(privateEnvironment);
  tracer.visit(homeObject);
  tracer.visit(scriptOrModule);
  for (auto& field : fields) {
    if (field.key) field.key->trace(tracer);
    tracer.visit(field.privateName);
    tracer.visit(field.initializer);
  }
  for (auto& method : privateMethods) {
    tracer.visit(method.key);
    tracer.visit(method.value);
    tracer.visit(method.getter);
    tracer.visit(method.setter);
  }
  tracer.visit(classFieldInitializerName);
}

String* ECMAScriptFunction::sourceText() const { return newString(sourceTextView); }

// 10.2.1 [[Call]]
Result<Value> ECMAScriptFunction::Call(Value thisArgument, Args args) {
  TRY(CheckStack());
  heap().maybeCollect();
  if (codeKind == CodeKind::DefaultConstructor) {
    ExecutionContext context;
    context.realm = realm();
    context.function = this;
    ContextScope scope(&context);
    return ThrowTypeError("Class constructor cannot be invoked without 'new'");
  }
  InterpContext calleeContext;
  PrepareForOrdinaryCall(this, Value::undefined(), calleeContext);
  ContextScope scope(&calleeContext);
  if (isClassConstructor) return ThrowTypeError("Class constructor cannot be invoked without 'new'");
  OrdinaryCallBindThis(this, calleeContext, thisArgument);
  return OrdinaryCallEvaluateBody(this, args);
}

// 10.2.2 [[Construct]]
Result<Object*> ECMAScriptFunction::Construct(Args args, Object* newTarget) {
  TRY(CheckStack());
  heap().maybeCollect();
  if (codeKind == CodeKind::DefaultConstructor) {
    ExecutionContext context;
    context.realm = realm();
    context.function = this;
    ContextScope scope(&context);
    return defaultConstructorBehaviour(this, args, newTarget);
  }
  Object* thisArgument = nullptr;
  if (!derived) thisArgument = TRY(OrdinaryCreateFromConstructor<Object>(newTarget, Intrinsic::ObjectPrototype));
  InterpContext calleeContext;
  PrepareForOrdinaryCall(this, Value(newTarget), calleeContext);
  auto* constructorEnv = static_cast<FunctionEnvironment*>(calleeContext.lexicalEnvironment);
  Result<Value> result = Value::undefined();
  {
    ContextScope scope(&calleeContext);
    if (!derived) {
      OrdinaryCallBindThis(this, calleeContext, Value(thisArgument));
      TRY(InitializeInstanceElements(thisArgument, this));
    }
    result = OrdinaryCallEvaluateBody(this, args);
  }
  if (result.isThrow()) return Throw{result.thrown()};
  Value value = result.value();
  if (value.isObject()) return value.asObject();
  if (!derived) return thisArgument;
  if (!value.isUndefined()) return ThrowTypeError("Derived constructors may only return object or undefined");
  Value thisBinding = TRY(constructorEnv->GetThisBinding());
  return thisBinding.asObject();
}

// 10.2.1.4 OrdinaryCallEvaluateBody / EvaluateBody
Result<Value> OrdinaryCallEvaluateBody(ECMAScriptFunction* F, Args args) {
  auto* context = runningContext();
  switch (F->codeKind) {
    case CodeKind::FieldInitializer: {
      const ast::Node* initializer = F->code;
      if (IsAnonymousFunctionDefinition(initializer)) return NamedEvaluation(initializer, F->classFieldInitializerName);
      return evaluate(initializer);
    }
    case CodeKind::StaticBlock: {
      TRY(FunctionDeclarationInstantiation(F, Args()));
      Environment* lexEnv = context->lexicalEnvironment;
      TRY(DisposeResources(lexEnv, evaluateStatementList(static_cast<const ast::StaticBlock*>(F->code)->body)));
      return Value::undefined();
    }
    case CodeKind::DefaultConstructor:
      return Value::undefined();  // handled by [[Call]] / [[Construct]]
    case CodeKind::Function:
      break;
  }
  const ast::Function* function = F->function;
  if (function->generator && function->async) {
    TRY(FunctionDeclarationInstantiation(F, args));
    return startAsyncGeneratorBody(F, context);
  }
  if (function->generator) {
    TRY(FunctionDeclarationInstantiation(F, args));
    return startGeneratorBody(F, context);
  }
  if (function->async) return startAsyncFunctionBody(F, context, args);  // runs FunctionDeclarationInstantiation
  TRY(FunctionDeclarationInstantiation(F, args));
  return runFunctionBody(F, args);
}

// The statements (or concise body) of a function after FunctionDeclarationInstantiation.
Result<Value> runFunctionBody(ECMAScriptFunction* F, Args) {
  const ast::Function* function = F->function;
  if (function->expression) return evaluate(function->body);
  Environment* lexEnv = runningContext()->lexicalEnvironment;
  Completion completion =
      TRY(DisposeResources(lexEnv, evaluateStatementList(function->body->as<ast::BlockStatement>()->body)));
  if (completion.type == CompletionType::Return) return completion.value;
  return Value::undefined();
}

Result<Object*> OrdinaryCreateFromConstructorForFunction(Object* constructor, Intrinsic fallback) {
  return OrdinaryCreateFromConstructor<Object>(constructor, fallback);
}

// 10.2.3 OrdinaryFunctionCreate
ECMAScriptFunction* OrdinaryFunctionCreate(Object* prototype, const ast::Function* function, bool lexicalThis,
                                           Environment* env, PrivateEnvironment* privateEnv) {
  auto* F = snowman::make<ECMAScriptFunction>(prototype, vm().currentRealm());
  F->codeKind = CodeKind::Function;
  F->function = function;
  F->code = function;
  F->sourceTextView = function->sourceText;
  F->strict = function->strict;
  F->thisMode = lexicalThis ? ThisMode::Lexical : F->strict ? ThisMode::Strict : ThisMode::Global;
  F->environment = env;
  F->privateEnvironment = privateEnv;
  F->scriptOrModule = GetActiveScriptOrModule();
  F->classFieldInitializerName = Value::empty();
  SetFunctionLength(F, function->expectedArgumentCount);
  return F;
}

// 10.2.5 MakeConstructor
void MakeConstructor(ECMAScriptFunction* F, bool writablePrototype, Object* prototype) {
  F->hasConstruct = true;
  if (!prototype) {
    prototype = OrdinaryObjectCreate(F->realm()->intrinsic(Intrinsic::ObjectPrototype));
    defineValue(prototype, PropertyKey(u"constructor"), Value(F),
                writablePrototype ? Attr::WritableConfigurable : Attr::Configurable);
  }
  defineValue(F, PropertyKey(u"prototype"), Value(prototype), writablePrototype ? Attr::Writable : Attr::None);
}

// 10.2.6 MakeClassConstructor
void MakeClassConstructor(ECMAScriptFunction* F) { F->isClassConstructor = true; }

// 10.2.7 MakeMethod
void MakeMethod(ECMAScriptFunction* F, Object* homeObject) { F->homeObject = homeObject; }

// 10.2.9 SetFunctionName
void SetFunctionName(Object* F, Value name, std::u16string_view prefix) {
  String* text;
  if (name.isSymbol()) {
    Value description = name.asSymbol()->description();
    if (description.isUndefined()) {
      text = emptyString();
    } else {
      std::u16string bracketed = u"[";
      bracketed += description.asString()->view();
      bracketed += u"]";
      text = newString(std::move(bracketed));
    }
  } else {
    text = name.asString();
  }
  if (!prefix.empty()) {
    std::u16string prefixed(prefix);
    prefixed += u" ";
    prefixed += text->view();
    text = newString(std::move(prefixed));
  }
  if (auto* builtin = F->as<BuiltinFunction>()) builtin->initialName = Value(text);
  defineValue(F, PropertyKey(u"name"), Value(text), Attr::Configurable);
}

// 10.2.8 DefineMethodProperty (private methods are handled by the class evaluator)
Result<void> DefineMethodProperty(Object* homeObject, Value key, Object* closure, bool enumerable) {
  PropertyKey propertyKey = TRY(ToPropertyKey(key));
  Attr attrs = enumerable ? Attr::All : Attr::WritableConfigurable;
  return DefinePropertyOrThrow(homeObject, propertyKey, PropertyDescriptor::data(Value(closure), attrs));
}

Object* functionPrototypeFor(Realm* realm, const ast::Function* function) {
  if (function->async && function->generator) return realm->intrinsic(Intrinsic::AsyncGeneratorFunctionPrototype);
  if (function->generator) return realm->intrinsic(Intrinsic::GeneratorFunctionPrototype);
  if (function->async) return realm->intrinsic(Intrinsic::AsyncFunctionPrototype);
  return realm->intrinsic(Intrinsic::FunctionPrototype);
}

namespace {

// The "prototype" property / [[Construct]] a new function gets by kind (InstantiateFunctionObject and the
// expression forms).
void finishFunctionObject(ECMAScriptFunction* F, const ast::Function* function) {
  Realm* realm = F->realm();
  if (function->generator) {
    Intrinsic prototypeId =
        function->async ? Intrinsic::AsyncGeneratorPrototype : Intrinsic::GeneratorPrototype;
    Object* prototype = OrdinaryObjectCreate(realm->intrinsic(prototypeId));
    defineValue(F, PropertyKey(u"prototype"), Value(prototype), Attr::Writable);
    return;
  }
  if (function->async || function->isArrow() || function->kind != ast::FunctionKind::Normal) return;
  MakeConstructor(F);
}

}  // namespace

// InstantiateFunctionObject (FunctionDeclaration and its generator / async forms)
Result<Value> InstantiateFunctionObject(const ast::Function* function, Environment* env,
                                        PrivateEnvironment* privateEnv) {
  Realm* realm = vm().currentRealm();
  ECMAScriptFunction* F = OrdinaryFunctionCreate(functionPrototypeFor(realm, function), function, false, env,
                                                 privateEnv);
  Value name = function->id ? Value(identifierName(function->id)) : Value(atom(u"default"));
  SetFunctionName(F, name);
  finishFunctionObject(F, function);
  return Value(F);
}

// InstantiateOrdinaryFunctionExpression / InstantiateGeneratorFunctionExpression / ... / arrow functions.
Result<Value> InstantiateFunctionExpression(const ast::Function* function, Value name) {
  InterpContext* context = runningContext();
  Environment* env = context->lexicalEnvironment;
  PrivateEnvironment* privateEnv = context->privateEnvironment;
  Realm* realm = vm().currentRealm();
  bool arrow = function->isArrow();
  DeclarativeEnvironment* funcEnv = nullptr;
  String* ownName = nullptr;
  if (!arrow && function->id) {
    ownName = identifierName(function->id);
    funcEnv = snowman::make<DeclarativeEnvironment>(env);
    TRY(funcEnv->CreateImmutableBinding(ownName, false));
    env = funcEnv;
    name = Value(ownName);
  }
  if (name.isEmpty()) name = Value(emptyString());
  ECMAScriptFunction* closure =
      OrdinaryFunctionCreate(functionPrototypeFor(realm, function), function, arrow, env, privateEnv);
  SetFunctionName(closure, name);
  finishFunctionObject(closure, function);
  if (funcEnv) TRY(funcEnv->InitializeBinding(ownName, Value(closure)));
  return Value(closure);
}

// DefineMethod (15.4.4) and the closure part of the generator / async method forms. The caller names it.
Result<ECMAScriptFunction*> DefineMethod(const ast::Function* function, Object* object, Object* functionPrototype) {
  InterpContext* context = runningContext();
  Realm* realm = vm().currentRealm();
  Object* prototype = functionPrototype ? functionPrototype : functionPrototypeFor(realm, function);
  ECMAScriptFunction* closure = OrdinaryFunctionCreate(prototype, function, false, context->lexicalEnvironment,
                                                       context->privateEnvironment);
  MakeMethod(closure, object);
  if (function->generator) {
    Intrinsic prototypeId =
        function->async ? Intrinsic::AsyncGeneratorPrototype : Intrinsic::GeneratorPrototype;
    Object* instancePrototype = OrdinaryObjectCreate(realm->intrinsic(prototypeId));
    defineValue(closure, PropertyKey(u"prototype"), Value(instancePrototype), Attr::Writable);
  }
  return closure;
}

namespace {

// IteratorBindingInitialization of FormalParameters over the argument list (CreateListIteratorRecord is
// unobservable, so this indexes the arguments directly).
Result<void> bindFormals(const std::vector<ast::Node*>& formals, Args args, Environment* environment) {
  for (size_t i = 0; i < formals.size(); i++) {
    const ast::Node* param = formals[i];
    if (param->type == ast::NodeType::RestElement) {
      Object* rest = CreateArrayFromList(args.slice(i));
      TRY(BindingInitialization(param->as<ast::RestElement>()->argument, Value(rest), environment));
      break;
    }
    Value value = args[i];
    if (param->type == ast::NodeType::AssignmentPattern) {
      auto* assignment = param->as<ast::AssignmentPattern>();
      if (value.isUndefined()) {
        if (assignment->left->type == ast::NodeType::Identifier) {
          value = TRY(evaluateNamed(assignment->right, Value(identifierName(assignment->left->as<ast::Identifier>()))));
        } else {
          value = TRY(evaluate(assignment->right));
        }
      }
      TRY(BindingInitialization(assignment->left, value, environment));
      continue;
    }
    TRY(BindingInitialization(param, value, environment));
  }
  return {};
}

bool containsName(const std::vector<String*>& names, String* name) {
  for (String* n : names) {
    if (n == name) return true;
  }
  return false;
}

}  // namespace

Result<void> IteratorBindingInitialization(const std::vector<ast::Node*>& formals, Args args,
                                           Environment* environment) {
  return bindFormals(formals, args, environment);
}

// 10.2.11 FunctionDeclarationInstantiation
Result<void> FunctionDeclarationInstantiation(ECMAScriptFunction* func, Args args) {
  InterpContext* calleeContext = runningContext();
  const ast::VarScope* code = varScopeOf(func);
  const ast::Function* function = func->codeKind == CodeKind::Function ? func->function : nullptr;
  bool strict = func->strict;
  const std::vector<ast::Node*>& formals = formalsOf(func);
  bool hasDuplicates = function && function->hasDuplicateParameters;
  bool simpleParameterList = !function || function->hasSimpleParameterList;
  bool hasParameterExpressions = function && function->hasParameterExpressions;

  std::vector<String*> parameterNames;
  if (function) {
    for (const auto& name : function->parameterNames) parameterNames.push_back(atomFor(name));
  }
  std::vector<String*> functionNames;
  for (const ast::FunctionDeclaration* f : code->functionsToInitialize) functionNames.push_back(identifierName(f->id));

  bool argumentsObjectNeeded = function && function->argumentsObjectNeeded && func->thisMode != ThisMode::Lexical;
  // The arguments object is unobservable when nothing can name it.
  if (argumentsObjectNeeded && !function->referencesArguments && !code->hasDirectEval) argumentsObjectNeeded = false;

  Environment* env;
  if (strict || !hasParameterExpressions) {
    env = calleeContext->lexicalEnvironment;
  } else {
    env = snowman::make<DeclarativeEnvironment>(calleeContext->lexicalEnvironment);
    calleeContext->lexicalEnvironment = env;
  }
  auto* declEnv = static_cast<DeclarativeEnvironment*>(env);

  for (String* paramName : parameterNames) {
    if (!declEnv->hasBindingDirect(paramName)) {
      TRY(declEnv->CreateMutableBinding(paramName, false));
      if (hasDuplicates) TRY(declEnv->InitializeBinding(paramName, Value::undefined()));
    }
  }

  std::vector<String*> parameterBindings = parameterNames;
  if (argumentsObjectNeeded) {
    Object* ao;
    if (strict || !simpleParameterList) {
      ao = CreateUnmappedArgumentsObject(args);
    } else {
      ao = CreateMappedArgumentsObject(func, function->parameterNames, args, env);
    }
    String* argumentsName = atom(u"arguments");
    if (strict) {
      TRY(declEnv->CreateImmutableBinding(argumentsName, false));
    } else {
      TRY(declEnv->CreateMutableBinding(argumentsName, false));
    }
    TRY(declEnv->InitializeBinding(argumentsName, Value(ao)));
    parameterBindings.push_back(argumentsName);
  }

  TRY(bindFormals(formals, args, hasDuplicates ? nullptr : env));

  Environment* varEnv;
  std::vector<String*> instantiatedVarNames;
  if (!hasParameterExpressions) {
    instantiatedVarNames = parameterBindings;
    for (const auto& varName : code->varNames) {
      String* n = atomFor(varName);
      if (!containsName(instantiatedVarNames, n)) {
        instantiatedVarNames.push_back(n);
        TRY(declEnv->CreateMutableBinding(n, false));
        TRY(declEnv->InitializeBinding(n, Value::undefined()));
      }
    }
    varEnv = env;
  } else {
    auto* newVarEnv = snowman::make<DeclarativeEnvironment>(env);
    varEnv = newVarEnv;
    calleeContext->variableEnvironment = varEnv;
    for (const auto& varName : code->varNames) {
      String* n = atomFor(varName);
      if (containsName(instantiatedVarNames, n)) continue;
      instantiatedVarNames.push_back(n);
      TRY(newVarEnv->CreateMutableBinding(n, false));
      Value initialValue = Value::undefined();
      if (containsName(parameterBindings, n) && !containsName(functionNames, n)) {
        initialValue = TRY(env->GetBindingValue(n, false));
      }
      TRY(newVarEnv->InitializeBinding(n, initialValue));
    }
  }

  // Annex B.3.2.1
  auto* varDeclEnv = static_cast<DeclarativeEnvironment*>(varEnv);
  for (ast::FunctionDeclaration* f : code->annexBFunctions) {
    String* F = identifierName(f->id);
    if (!containsName(instantiatedVarNames, F) && F != atom(u"arguments")) {
      TRY(varDeclEnv->CreateMutableBinding(F, false));
      TRY(varDeclEnv->InitializeBinding(F, Value::undefined()));
      instantiatedVarNames.push_back(F);
    }
    f->interpCache = reinterpret_cast<void*>(1);  // evaluate it with the B.3.2.1 steps
  }

  Environment* lexEnv;
  if (!strict) {
    lexEnv = snowman::make<DeclarativeEnvironment>(varEnv);
  } else {
    lexEnv = varEnv;
  }
  calleeContext->lexicalEnvironment = lexEnv;
  auto* lexDeclEnv = static_cast<DeclarativeEnvironment*>(lexEnv);
  for (const auto& d : code->lexicallyScopedDeclarations) {
    for (const auto& dn : d.boundNames) {
      if (d.isConstant) {
        TRY(lexDeclEnv->CreateImmutableBinding(atomFor(dn), true));
      } else {
        TRY(lexDeclEnv->CreateMutableBinding(atomFor(dn), false));
      }
    }
  }

  PrivateEnvironment* privateEnv = calleeContext->privateEnvironment;
  for (const ast::FunctionDeclaration* f : code->functionsToInitialize) {
    String* fn = identifierName(f->id);
    Value fo = TRY(InstantiateFunctionObject(f, lexEnv, privateEnv));
    TRY(varEnv->SetMutableBinding(fn, fo, false));
  }
  return {};
}

}  // namespace snowman::interp
