// Scripts (16.1), eval (19.2.1) and the interpreter's per-realm state.
#include "interp/interpreter.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/vm.h"

namespace snowman::interp {

void ScriptRecord::trace(Tracer& tracer) {
  tracer.visit(realm);
  for (auto& entry : loadedModules) tracer.visit(reinterpret_cast<Cell*>(entry.second));
}

void RealmState::trace(Tracer& tracer) {
  for (auto& entry : templateMap) tracer.visit(entry.second);
  for (auto& entry : moduleMap) tracer.visit(reinterpret_cast<Cell*>(entry.second));
  tracer.visit(host262);
}

RealmState* realmState(Realm* realm) { return static_cast<RealmState*>(realm->hostDefined); }

GlobalEnvironment* globalEnvironment(Realm* realm) { return static_cast<GlobalEnvironment*>(realm->globalEnv); }

// InitializeHostDefinedRealm's remaining steps for a realm from CreateRealm().
void setUpRealm(Realm* realm) {
  Object* global = realm->globalObject();
  realm->globalEnv = snowman::make<GlobalEnvironment>(global, global);
  realm->hostDefined = snowman::make<RealmState>();
  installHostBindings(realm);
}

Throw throwParseError(const parser::ParseError& error) { return ThrowSyntaxError(error.message); }

// 16.1.5 ParseScript
Result<ScriptRecord*> ParseScript(std::u16string source, Realm* realm) {
  parser::ParseResult result = parser::parseProgram(std::move(source));
  if (!result.ok()) return throwParseError(*result.error);
  return snowman::make<ScriptRecord>(realm, result.program);
}

// 16.1.6 ScriptEvaluation
Result<Value> ScriptEvaluation(ScriptRecord* scriptRecord) {
  GlobalEnvironment* globalEnv = globalEnvironment(scriptRecord->realm);
  InterpContext scriptContext;
  scriptContext.realm = scriptRecord->realm;
  scriptContext.scriptOrModule = scriptRecord;
  scriptContext.variableEnvironment = globalEnv;
  scriptContext.lexicalEnvironment = globalEnv;
  scriptContext.strict = scriptRecord->program->strict;
  ContextScope scope(&scriptContext);
  TRY(GlobalDeclarationInstantiation(scriptRecord->program, globalEnv));
  Completion completion = TRY(evaluateStatementList(scriptRecord->program->body));
  return completion.value.isEmpty() ? Value::undefined() : completion.value;
}

namespace {

void collectPrivateNames(PrivateEnvironment* env, std::vector<std::u16string>& out) {
  for (; env; env = env->outer) {
    for (PrivateName* name : env->names) out.emplace_back(name->description->view());
  }
}

}  // namespace

// 19.2.1.1 PerformEval
Result<Value> PerformEval(Value x, bool strictCaller, bool direct) {
  if (!x.isString()) return x;
  Realm* evalRealm = vm().currentRealm();
  parser::ParseOptions options;
  options.eval = true;
  options.strict = strictCaller;
  InterpContext* caller = direct ? runningContext() : nullptr;
  if (direct) {
    Environment* thisEnvRec = GetThisEnvironment(caller->lexicalEnvironment);
    if (thisEnvRec->kind() == Environment::Kind::Function) {
      auto* functionEnv = static_cast<FunctionEnvironment*>(thisEnvRec);
      auto* F = static_cast<ECMAScriptFunction*>(functionEnv->functionObject);
      options.inFunction = true;
      options.inMethod = functionEnv->HasSuperBinding();
      options.inDerivedConstructor = F->derived;
      options.inClassFieldInitializer = !F->classFieldInitializerName.isEmpty();
    }
    collectPrivateNames(caller->privateEnvironment, options.privateNames);
  }
  parser::ParseResult parsed = parser::parseProgram(std::u16string(x.asString()->view()), options);
  if (!parsed.ok()) return throwParseError(*parsed.error);
  ast::Program* body = parsed.program;
  bool strictEval = strictCaller || body->strict;

  DeclarativeEnvironment* lexEnv;
  Environment* varEnv;
  PrivateEnvironment* privateEnv;
  if (direct) {
    lexEnv = snowman::make<DeclarativeEnvironment>(caller->lexicalEnvironment);
    varEnv = caller->variableEnvironment;
    privateEnv = caller->privateEnvironment;
  } else {
    GlobalEnvironment* globalEnv = globalEnvironment(evalRealm);
    lexEnv = snowman::make<DeclarativeEnvironment>(globalEnv);
    varEnv = globalEnv;
    privateEnv = nullptr;
  }
  if (strictEval) varEnv = lexEnv;

  InterpContext evalContext;
  evalContext.realm = evalRealm;
  evalContext.scriptOrModule = caller ? caller->scriptOrModule : nullptr;
  evalContext.variableEnvironment = varEnv;
  evalContext.lexicalEnvironment = lexEnv;
  evalContext.privateEnvironment = privateEnv;
  evalContext.strict = strictEval;
  if (caller) {
    // Eval code inside a generator / async body suspends with it (await / yield are not allowed in eval
    // code itself, but the context must still say which body it belongs to).
    evalContext.suspendOwner = caller->suspendOwner;
    evalContext.suspend = caller->suspend;
  }
  ContextScope scope(&evalContext);
  TRY(EvalDeclarationInstantiation(body, varEnv, lexEnv, privateEnv, strictEval));
  Completion completion = TRY(evaluateStatementList(body->body));
  return completion.value.isEmpty() ? Value::undefined() : completion.value;
}

}  // namespace snowman::interp
