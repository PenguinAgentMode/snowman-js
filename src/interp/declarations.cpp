// Declaration instantiation: GlobalDeclarationInstantiation (16.1.7), BlockDeclarationInstantiation (14.2.3),
// EvalDeclarationInstantiation (19.2.1.3), with their Annex B.3.2 changes.
#include "interp/interpreter.h"
#include "runtime/errors.h"
#include "runtime/operations.h"

namespace snowman::interp {

using namespace ast;

namespace {

bool contains(const std::vector<String*>& names, String* name) {
  for (String* n : names) {
    if (n == name) return true;
  }
  return false;
}

Throw redeclaration(String* name) {
  return ThrowSyntaxError("Identifier '" + name->toUtf8() + "' has already been declared");
}

// The bound names of a var-scoped VariableDeclarator (its pattern's names).
void collectBoundNames(const Node* pattern, std::vector<String*>& out) {
  switch (pattern->type) {
    case NodeType::Identifier:
      out.push_back(identifierName(pattern->as<Identifier>()));
      break;
    case NodeType::AssignmentPattern:
      collectBoundNames(pattern->as<AssignmentPattern>()->left, out);
      break;
    case NodeType::RestElement:
      collectBoundNames(pattern->as<RestElement>()->argument, out);
      break;
    case NodeType::ArrayPattern:
      for (const Node* element : pattern->as<ArrayPattern>()->elements) {
        if (element) collectBoundNames(element, out);
      }
      break;
    case NodeType::ObjectPattern:
      for (const Node* member : pattern->as<ObjectPattern>()->properties) {
        if (member->type == NodeType::RestElement) {
          collectBoundNames(member, out);
        } else {
          collectBoundNames(member->as<Property>()->value, out);
        }
      }
      break;
    default:
      break;
  }
}

}  // namespace

// 16.1.7 GlobalDeclarationInstantiation
Result<void> GlobalDeclarationInstantiation(const Program* script, GlobalEnvironment* env) {
  for (const auto& d : script->lexicallyScopedDeclarations) {
    for (const auto& dn : d.boundNames) {
      String* name = atomFor(dn);
      if (env->HasLexicalDeclaration(name)) return redeclaration(name);
      bool hasRestrictedGlobal = TRY(env->HasRestrictedGlobalProperty(name));
      if (hasRestrictedGlobal) return redeclaration(name);
    }
  }
  for (const auto& varName : script->varNames) {
    String* name = atomFor(varName);
    if (env->HasLexicalDeclaration(name)) return redeclaration(name);
  }
  // functionsToInitialize (the parser's list is the last declaration per name, in source order); the
  // CanDeclareGlobalFunction checks run in reverse source order as in the spec.
  std::vector<String*> declaredFunctionNames;
  const auto& functions = script->functionsToInitialize;
  for (size_t i = functions.size(); i-- > 0;) {
    String* fn = identifierName(functions[i]->id);
    if (contains(declaredFunctionNames, fn)) continue;
    bool fnDefinable = TRY(env->CanDeclareGlobalFunction(fn));
    if (!fnDefinable) return ThrowTypeError("Cannot declare global function '" + fn->toUtf8() + "'");
    declaredFunctionNames.push_back(fn);
  }
  std::vector<String*> declaredVarNames;
  for (const Node* d : script->varScopedDeclarations) {
    if (d->type != NodeType::VariableDeclarator) continue;
    std::vector<String*> bound;
    collectBoundNames(d->as<VariableDeclarator>()->id, bound);
    for (String* vn : bound) {
      if (contains(declaredFunctionNames, vn)) continue;
      bool vnDefinable = TRY(env->CanDeclareGlobalVar(vn));
      if (!vnDefinable) return ThrowTypeError("Cannot declare global variable '" + vn->toUtf8() + "'");
      if (!contains(declaredVarNames, vn)) declaredVarNames.push_back(vn);
    }
  }

  // Annex B.3.2.2
  std::vector<String*> declaredFunctionOrVarNames = declaredFunctionNames;
  declaredFunctionOrVarNames.insert(declaredFunctionOrVarNames.end(), declaredVarNames.begin(), declaredVarNames.end());
  for (FunctionDeclaration* f : script->annexBFunctions) {
    String* F = identifierName(f->id);
    if (env->HasLexicalDeclaration(F)) continue;
    bool fnDefinable = TRY(env->CanDeclareGlobalVar(F));
    if (!fnDefinable) continue;
    if (!contains(declaredFunctionOrVarNames, F)) {
      TRY(env->CreateGlobalVarBinding(F, false));
      declaredFunctionOrVarNames.push_back(F);
    }
    f->interpCache = reinterpret_cast<void*>(1);
  }

  for (const auto& d : script->lexicallyScopedDeclarations) {
    for (const auto& dn : d.boundNames) {
      if (d.isConstant) {
        TRY(env->CreateImmutableBinding(atomFor(dn), true));
      } else {
        TRY(env->CreateMutableBinding(atomFor(dn), false));
      }
    }
  }
  for (const FunctionDeclaration* f : functions) {
    String* fn = identifierName(f->id);
    Value fo = TRY(InstantiateFunctionObject(f, env, nullptr));
    TRY(env->CreateGlobalFunctionBinding(fn, fo, false));
  }
  for (String* vn : declaredVarNames) TRY(env->CreateGlobalVarBinding(vn, false));
  return {};
}

// 14.2.3 BlockDeclarationInstantiation, with B.3.2.6 (duplicate sloppy function declarations in a block).
void BlockDeclarationInstantiation(const std::vector<LexicalDeclaration>& declarations, DeclarativeEnvironment* env) {
  PrivateEnvironment* privateEnv = runningContext()->privateEnvironment;
  for (const auto& d : declarations) {
    for (const auto& dn : d.boundNames) {
      String* name = atomFor(dn);
      if (d.isConstant) {
        (void)env->CreateImmutableBinding(name, true);
      } else if (!env->hasBindingDirect(name)) {
        (void)env->CreateMutableBinding(name, false);
      }
    }
    if (d.node->type == NodeType::FunctionDeclaration) {
      const auto* f = d.node->as<FunctionDeclaration>();
      String* fn = atomFor(d.boundNames[0]);
      Value fo = InstantiateFunctionObject(f, env, privateEnv).release();
      auto* binding = env->find(fn);
      if (binding->value.isEmpty()) {
        binding->value = fo;
      } else {
        (void)env->SetMutableBinding(fn, fo, false);
      }
    }
  }
}

// 19.2.1.3 EvalDeclarationInstantiation, with B.3.2.3 and B.3.4.
Result<void> EvalDeclarationInstantiation(const Program* body, Environment* varEnv, DeclarativeEnvironment* lexEnv,
                                          PrivateEnvironment* privateEnv, bool strict) {
  bool varEnvIsGlobal = varEnv->kind() == Environment::Kind::Global;
  auto* globalEnv = varEnvIsGlobal ? static_cast<GlobalEnvironment*>(varEnv) : nullptr;
  std::vector<String*> varNames;
  for (const auto& name : body->varNames) varNames.push_back(atomFor(name));

  if (!strict) {
    if (globalEnv) {
      for (String* name : varNames) {
        if (globalEnv->HasLexicalDeclaration(name)) return redeclaration(name);
      }
    }
    for (Environment* thisEnv = lexEnv; thisEnv != varEnv; thisEnv = thisEnv->outer) {
      if (thisEnv->kind() == Environment::Kind::Object) continue;
      auto* declEnv = static_cast<DeclarativeEnvironment*>(thisEnv);
      for (String* name : varNames) {
        if (!declEnv->hasBindingDirect(name)) continue;
        // B.3.4: a var may redeclare a catch parameter (not a function declaration's name).
        bool isFunctionName = false;
        for (const FunctionDeclaration* f : body->functionsToInitialize) {
          if (identifierName(f->id) == name) isFunctionName = true;
        }
        if (declEnv->isCatchEnvironment && !isFunctionName) continue;
        return redeclaration(name);
      }
    }
  }

  std::vector<String*> declaredFunctionNames;
  const auto& functions = body->functionsToInitialize;
  for (size_t i = functions.size(); i-- > 0;) {
    String* fn = identifierName(functions[i]->id);
    if (contains(declaredFunctionNames, fn)) continue;
    if (globalEnv) {
      bool fnDefinable = TRY(globalEnv->CanDeclareGlobalFunction(fn));
      if (!fnDefinable) return ThrowTypeError("Cannot declare global function '" + fn->toUtf8() + "'");
    }
    declaredFunctionNames.push_back(fn);
  }
  std::vector<String*> declaredVarNames;
  for (const Node* d : body->varScopedDeclarations) {
    if (d->type != NodeType::VariableDeclarator) continue;
    std::vector<String*> bound;
    collectBoundNames(d->as<VariableDeclarator>()->id, bound);
    for (String* vn : bound) {
      if (contains(declaredFunctionNames, vn)) continue;
      if (globalEnv) {
        bool vnDefinable = TRY(globalEnv->CanDeclareGlobalVar(vn));
        if (!vnDefinable) return ThrowTypeError("Cannot declare global variable '" + vn->toUtf8() + "'");
      }
      if (!contains(declaredVarNames, vn)) declaredVarNames.push_back(vn);
    }
  }

  // Annex B.3.2.3
  if (!strict) {
    std::vector<String*> declaredFunctionOrVarNames = declaredFunctionNames;
    declaredFunctionOrVarNames.insert(declaredFunctionOrVarNames.end(), declaredVarNames.begin(),
                                      declaredVarNames.end());
    for (FunctionDeclaration* f : body->annexBFunctions) {
      String* F = identifierName(f->id);
      bool bindingExists = false;
      for (Environment* thisEnv = lexEnv; thisEnv != varEnv; thisEnv = thisEnv->outer) {
        if (thisEnv->kind() == Environment::Kind::Object) continue;
        if (static_cast<DeclarativeEnvironment*>(thisEnv)->hasBindingDirect(F)) bindingExists = true;
      }
      bool fnDefinable = true;
      if (!bindingExists && globalEnv) {
        fnDefinable = globalEnv->HasLexicalDeclaration(F) ? false : TRY(globalEnv->CanDeclareGlobalVar(F));
      }
      if (bindingExists || !fnDefinable) continue;
      if (!contains(declaredFunctionOrVarNames, F)) {
        if (globalEnv) {
          TRY(globalEnv->CreateGlobalVarBinding(F, true));
        } else if (!TRY(varEnv->HasBinding(F))) {
          TRY(varEnv->CreateMutableBinding(F, true));
          TRY(varEnv->InitializeBinding(F, Value::undefined()));
        }
        declaredFunctionOrVarNames.push_back(F);
      }
      f->interpCache = reinterpret_cast<void*>(1);
    }
  }

  for (const auto& d : body->lexicallyScopedDeclarations) {
    for (const auto& dn : d.boundNames) {
      if (d.isConstant) {
        TRY(lexEnv->CreateImmutableBinding(atomFor(dn), true));
      } else {
        TRY(lexEnv->CreateMutableBinding(atomFor(dn), false));
      }
    }
  }
  for (const FunctionDeclaration* f : functions) {
    String* fn = identifierName(f->id);
    Value fo = TRY(InstantiateFunctionObject(f, lexEnv, privateEnv));
    if (globalEnv) {
      TRY(globalEnv->CreateGlobalFunctionBinding(fn, fo, true));
    } else if (!TRY(varEnv->HasBinding(fn))) {
      TRY(varEnv->CreateMutableBinding(fn, true));
      TRY(varEnv->InitializeBinding(fn, fo));
    } else {
      TRY(varEnv->SetMutableBinding(fn, fo, false));
    }
  }
  for (String* vn : declaredVarNames) {
    if (globalEnv) {
      TRY(globalEnv->CreateGlobalVarBinding(vn, true));
    } else if (!TRY(varEnv->HasBinding(vn))) {
      TRY(varEnv->CreateMutableBinding(vn, true));
      TRY(varEnv->InitializeBinding(vn, Value::undefined()));
    }
  }
  return {};
}

}  // namespace snowman::interp
