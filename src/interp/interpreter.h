// The tree-walking evaluator: execution contexts, completions, Reference Records and the spec operations the
// interpreter's files share. Each operation is named after ECMA-262.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "interp/environment.h"
#include "interp/function.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/operations.h"
#include "runtime/result.h"
#include "runtime/vm.h"

namespace snowman::interp {

class Coroutine;
class ModuleRecord;

// ---- Scripts and realms ----

// Script Record (16.1.4).
class ScriptRecord final : public Cell {
 public:
  ScriptRecord(Realm* realm, ast::Program* program) : realm(realm), program(program) {}
  void trace(Tracer& tracer) override;

  Realm* realm;
  ast::Program* program;
  // [[LoadedModules]]: specifier → module (dynamic import from this script).
  std::vector<std::pair<std::u16string, ModuleRecord*>> loadedModules;
};

// The interpreter's per-realm state (Realm::hostDefined): [[TemplateMap]], the module map, the host's $262.
class RealmState final : public Cell {
 public:
  void trace(Tracer& tracer) override;

  std::vector<std::pair<const ast::Node*, Object*>> templateMap;
  std::vector<std::pair<std::string, ModuleRecord*>> moduleMap;  // resolved path → module
  Object* host262 = nullptr;
};

RealmState* realmState(Realm* realm);
GlobalEnvironment* globalEnvironment(Realm* realm);

// InitializeHostDefinedRealm's interpreter part for a realm from CreateRealm(): the global environment, the
// realm state and the host bindings (host.cpp).
void setUpRealm(Realm* realm);
// The host's global bindings (print, $262) for `realm` (host.cpp).
void installHostBindings(Realm* realm);

// ---- Suspension (generators, async functions, async generators, module top-level await) ----

enum class ResumeKind : uint8_t { Normal, Throw, Return };

// The coroutine running a generator or async body, and the completion it is resumed with.
struct SuspendState {
  Coroutine* coroutine = nullptr;
  ResumeKind resumeKind = ResumeKind::Normal;
  Value resumeValue;
  // The value of a pending return completion travelling as the return marker (below).
  Value returnValue;
};

// A generator resumed with a return completion unwinds its body as a throw of the return marker (an empty
// Value, never a JS value); catch clauses don't catch it, finally blocks run, and the generator body turns it
// back into a return completion of SuspendState::returnValue.
inline Throw returnMarker() { return Throw{Value::empty()}; }
inline bool isReturnMarker(Value thrown) { return thrown.isEmpty(); }

// ---- Safepoints ----

// A safepoint (function entry, loop back-edge): collects when the heap's budget is spent, or every
// gcStressInterval safepoints when set (SNOWMAN_GC_STRESS=N, for finding unrooted cells).
extern unsigned gcStressInterval;
void stressCollect();
inline void safepoint() {
  if (gcStressInterval) return stressCollect();
  heap().maybeCollect();
}

// ---- Execution contexts ----

struct InterpContext : ExecutionContext {
  Environment* lexicalEnvironment = nullptr;
  Environment* variableEnvironment = nullptr;
  PrivateEnvironment* privateEnvironment = nullptr;
  Cell* scriptOrModule = nullptr;  // ScriptRecord, ModuleRecord, or nullptr
  bool strict = false;             // the code being evaluated is strict mode code
  // The generator / async function / async generator running in this context, if any.
  Cell* suspendOwner = nullptr;
  SuspendState* suspend = nullptr;
  // For the legacy f.caller and f.arguments: eval code (not a caller), and a function call's argument list.
  bool isEval = false;
  Args args;
};

// The running execution context as an interpreter context (nullptr when a built-in's context is running).
InterpContext* runningContext();
// GetActiveScriptOrModule
Cell* GetActiveScriptOrModule();

// Pushes a context for the lifetime of the scope.
class ContextScope {
 public:
  explicit ContextScope(ExecutionContext* context) { vm().pushContext(context); }
  ~ContextScope() { vm().popContext(); }
  ContextScope(const ContextScope&) = delete;
  ContextScope& operator=(const ContextScope&) = delete;
};

// ---- Completions ----

enum class CompletionType : uint8_t { Normal, Break, Continue, Return };

// A normal, break, continue or return completion (throw completions are Result's). value may be empty.
struct Completion {
  CompletionType type = CompletionType::Normal;
  Value value = Value::empty();
  const std::u16string* target = nullptr;  // break/continue label, nullptr for none

  bool isAbrupt() const { return type != CompletionType::Normal; }
  static Completion normal(Value value = Value::empty()) { return {CompletionType::Normal, value, nullptr}; }
};

// UpdateEmpty(completionRecord, value)
inline Completion UpdateEmpty(Completion completion, Value value) {
  if (completion.value.isEmpty()) completion.value = value;
  return completion;
}

// The label set of a labelled iteration/switch statement (pointers to label names).
using LabelSet = std::vector<const std::u16string*>;

// ---- Reference Records (6.2.5) ----

struct Reference {
  enum class Kind : uint8_t { Unresolvable, Environment, Property, Private };
  Kind kind = Kind::Unresolvable;
  Value base;                        // Property / Private: the base value
  Environment* env = nullptr;        // Environment
  String* name = nullptr;            // Environment / Unresolvable: the binding name
  Value propertyName;                // Property: [[ReferencedName]] before ToPropertyKey
  std::optional<PropertyKey> key;    // Property: [[ReferencedName]] as a property key, once converted
  PrivateName* privateName = nullptr;  // Private
  Value thisValue = Value::empty();  // a super reference's [[ThisValue]]
  bool strict = false;

  bool isPropertyReference() const { return kind == Kind::Property || kind == Kind::Private; }
  bool isSuperReference() const { return !thisValue.isEmpty(); }
};

Result<Value> GetValue(Reference& ref);
Result<void> PutValue(Reference& ref, Value value);
Result<void> InitializeReferencedBinding(Reference& ref, Value value);
Value GetThisValue(const Reference& ref);
Result<PropertyKey> referenceKey(Reference& ref);  // converts [[ReferencedName]] once (ToPropertyKey)

// GetIdentifierReference / ResolveBinding. env nullptr: the running context's LexicalEnvironment.
Result<Reference> ResolveBinding(String* name, Environment* env = nullptr);
Result<Value> ResolveThisBinding();
Value GetNewTarget();
Object* GetGlobalObject();

// ---- Evaluation ----

// The atom for an identifier node's name (cached on the node).
String* identifierName(const ast::Identifier* id);
// The atom for a name held by the AST (cached by address: never pass a string that does not live forever).
String* atomFor(const std::u16string& name);

// Evaluation followed by GetValue.
Result<Value> evaluate(const ast::Node* expression);
// Evaluation of an expression as a Reference (identifiers, member expressions, super properties, calls…).
Result<Reference> evaluateReference(const ast::Node* expression);
// NamedEvaluation of an anonymous function definition.
Result<Value> NamedEvaluation(const ast::Node* expression, Value name);
bool IsAnonymousFunctionDefinition(const ast::Node* expression);
// Evaluate `expression`, with NamedEvaluation(name) when it is an anonymous function definition.
Result<Value> evaluateNamed(const ast::Node* expression, Value name);

Result<Completion> evaluateStatement(const ast::Node* statement, const LabelSet* labels = nullptr);
Result<Completion> evaluateStatementList(const std::vector<ast::Node*>& statements);
// The body of a function / script as a statement list whose completion value matters.
Result<Completion> evaluateBlock(const ast::BlockStatement* block);

// ArgumentListEvaluation
Result<ValueVector> ArgumentListEvaluation(const std::vector<ast::Node*>& arguments);
// EvaluateCall(func, ref, arguments, tailPosition)
Result<Value> EvaluateCall(Value func, Value thisValue, const std::vector<ast::Node*>& arguments);
Result<Value> evaluatePropertyKey(const ast::Node* key, bool computed);  // a property name's value
Result<Value> GetTemplateObject(const ast::TemplateLiteral* templateLiteral);

// Binary operators (operators.cpp).
Result<Value> ApplyStringOrNumericBinaryOperator(Value lval, ast::BinaryOperator op, Value rval);
Result<Value> evaluateBinaryOperator(ast::BinaryOperator op, Value lval, Value rval);
Result<Value> evaluateUnaryNumeric(ast::UnaryOperator op, Value value);
ast::BinaryOperator binaryOperatorFor(ast::AssignmentOperator op);

// ---- Patterns (patterns.cpp) ----

// BindingInitialization(pattern, value, environment): environment nullptr means PutValue (var / parameters
// with no separate environment, and assignment targets in for-in/of).
Result<void> BindingInitialization(const ast::Node* pattern, Value value, Environment* environment);
// DestructuringAssignmentEvaluation of an assignment pattern.
Result<void> DestructuringAssignmentEvaluation(const ast::Node* pattern, Value value);
// IteratorBindingInitialization of a parameter list.
Result<void> IteratorBindingInitialization(const std::vector<ast::Node*>& formals, Args args,
                                           Environment* environment);

// ---- Functions (function.cpp) ----

ECMAScriptFunction* OrdinaryFunctionCreate(Object* prototype, const ast::Function* function, bool lexicalThis,
                                           Environment* env, PrivateEnvironment* privateEnv);
void MakeConstructor(ECMAScriptFunction* F, bool writablePrototype = true, Object* prototype = nullptr);
void MakeClassConstructor(ECMAScriptFunction* F);
void MakeMethod(ECMAScriptFunction* F, Object* homeObject);
// SetFunctionName(F, name, prefix): name is a String, a Symbol, or a PrivateName's description with '#'.
void SetFunctionName(Object* F, Value name, std::u16string_view prefix = {});
Result<void> DefineMethodProperty(Object* homeObject, Value key, Object* closure, bool enumerable);

Result<Value> InstantiateFunctionObject(const ast::Function* function, Environment* env,
                                        PrivateEnvironment* privateEnv);
// The evaluation of function / arrow / generator / async expressions, with an optional name.
Result<Value> InstantiateFunctionExpression(const ast::Function* function, Value name);
// MethodDefinition / object literal methods: DefineMethod and its generator / async variants.
Result<ECMAScriptFunction*> DefineMethod(const ast::Function* function, Object* object,
                                         Object* functionPrototype = nullptr);
// Sloppy plain functions' own "arguments" and "caller" (undefined, non-writable, non-configurable): an extension
// 17.1 allows, so legacy `f.caller` reads don't hit %ThrowTypeError%.
void AddLegacyFunctionProperties(ECMAScriptFunction* F);
// The %XFunction.prototype% intrinsic and instance prototype for a function's kind.
Object* functionPrototypeFor(Realm* realm, const ast::Function* function);

Result<void> FunctionDeclarationInstantiation(ECMAScriptFunction* func, Args args);
Result<Value> OrdinaryCallEvaluateBody(ECMAScriptFunction* F, Args args);
Result<Value> runFunctionBody(ECMAScriptFunction* F, Args args);  // the normal-function body, as a completion
Result<Object*> OrdinaryCreateFromConstructorForFunction(Object* constructor, Intrinsic fallback);

// ---- Arguments objects (arguments.cpp) ----

Object* CreateUnmappedArgumentsObject(Args args);
Object* CreateMappedArgumentsObject(ECMAScriptFunction* func, const std::vector<std::u16string>& formals,
                                    Args args, Environment* env);

// ---- Classes (class.cpp) ----

// ClassDefinitionEvaluation (classBinding: nullptr for none; name: the class name value, undefined for none).
Result<Value> ClassDefinitionEvaluation(const ast::Class* node, String* classBinding, Value className);
Result<Value> evaluateClass(const ast::Class* node, Value name);
Result<void> InitializeInstanceElements(Object* O, ECMAScriptFunction* constructor);
PrivateElement* PrivateElementFind(Object* O, PrivateName* P);
Result<void> PrivateFieldAdd(Object* O, PrivateName* P, Value value);
Result<void> PrivateMethodOrAccessorAdd(Object* O, const PrivateElement& method);
Result<Value> PrivateGet(Object* O, PrivateName* P);
Result<void> PrivateSet(Object* O, PrivateName* P, Value value);

// ---- Declaration instantiation (declarations.cpp) ----

Result<void> GlobalDeclarationInstantiation(const ast::Program* script, GlobalEnvironment* env);
void BlockDeclarationInstantiation(const std::vector<ast::LexicalDeclaration>& declarations,
                                   DeclarativeEnvironment* env);
Result<void> EvalDeclarationInstantiation(const ast::Program* body, Environment* varEnv,
                                          DeclarativeEnvironment* lexEnv, PrivateEnvironment* privateEnv,
                                          bool strict);

// ---- Scripts and eval (script.cpp) ----

// ParseScript: a SyntaxError (in the current realm) for a parse error.
Result<ScriptRecord*> ParseScript(std::u16string source, Realm* realm);
Result<Value> ScriptEvaluation(ScriptRecord* script);
Result<Value> PerformEval(Value x, bool strictCaller, bool direct);
// The SyntaxError for a ParseError, in the current realm.
Throw throwParseError(const parser::ParseError& error);

// ---- Generators and async (suspend.cpp) ----

// Yield(value) inside a generator; Await(value) inside an async function / generator / module.
Result<Value> Yield(Value value);
Result<Value> Await(Value value);
Result<Value> evaluateYield(const ast::YieldExpression* node);
Result<Value> evaluateYieldStar(const ast::YieldExpression* node);
// The kind of the running suspendable body.
bool inAsyncGenerator();
// Starts a generator / async body for F after FunctionDeclarationInstantiation.
Result<Value> startGeneratorBody(ECMAScriptFunction* F, InterpContext* context);
Result<Value> startAsyncFunctionBody(ECMAScriptFunction* F, InterpContext* context, Args args);
Result<Value> startAsyncGeneratorBody(ECMAScriptFunction* F, InterpContext* context);
// The body of a generator-like function, as the completion its coroutine finishes with.
Result<Value> evaluateGeneratorLikeBody(ECMAScriptFunction* F);

// AsyncIteratorClose / the async iterators used by for-await and yield* in async generators.
Result<void> AsyncIteratorClose(const IteratorRecord& record, Result<void> completion);
Result<IteratorRecord> GetAsyncIterator(Value object);

// ---- Static analysis (analysis.cpp) ----

// Whether evaluating `node` may capture the running environment: a function, class or arrow, or a direct
// eval candidate. Conservative: true for any node kind it does not know.
bool mayCaptureEnvironment(const ast::Node* node);
// mayCaptureEnvironment(loop), cached on the loop node.
bool loopMayCaptureEnvironment(const ast::Node* loop);

// ---- Modules (module.cpp) ----

Result<Value> evaluateImportCall(const ast::ImportExpression* node);
Result<Value> evaluateImportMeta();

}  // namespace snowman::interp
