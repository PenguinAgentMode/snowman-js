// Evaluation of expressions (ECMA-262 13), Reference Records (6.2.5) and identifier resolution (9.4).
#include <cmath>
#include <typeinfo>
#include <unordered_map>

#include "interp/interpreter.h"
#include "runtime/bigint.h"
#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/vm.h"

namespace snowman::interp {

using namespace ast;

// ---- Contexts ----

namespace {

// InterpContext is the only subclass of ExecutionContext: an exact type check is a cheap downcast.
InterpContext* asInterpContext(ExecutionContext* context) {
  return context && typeid(*context) == typeid(InterpContext) ? static_cast<InterpContext*>(context) : nullptr;
}

}  // namespace

InterpContext* runningContext() { return asInterpContext(vm().running); }

Cell* GetActiveScriptOrModule() {
  for (ExecutionContext* context = vm().running; context; context = context->previous) {
    if (auto* interp = asInterpContext(context)) {
      if (interp->scriptOrModule) return interp->scriptOrModule;
    }
  }
  return nullptr;
}

String* identifierName(const Identifier* id) {
  if (!id->interpCache) id->interpCache = atom(id->name);
  return static_cast<String*>(id->interpCache);
}

// AST strings are never freed and atoms live forever, so the atom can be cached by the string's address.
String* atomFor(const std::u16string& name) {
  static std::unordered_map<const std::u16string*, String*> cache;
  auto [it, inserted] = cache.try_emplace(&name, nullptr);
  if (inserted) it->second = atom(name);
  return it->second;
}

Object* GetGlobalObject() { return vm().currentRealm()->globalObject(); }

// ---- Identifier resolution ----

Result<Reference> ResolveBinding(String* name, Environment* env) {
  InterpContext* context = runningContext();
  if (!env) env = context->lexicalEnvironment;
  Reference ref;
  ref.name = name;
  ref.strict = context->strict;
  for (; env; env = env->outer) {
    bool exists;
    switch (env->kind()) {
      case Environment::Kind::Declarative:
      case Environment::Kind::Function:
      case Environment::Kind::Module:
        exists = static_cast<DeclarativeEnvironment*>(env)->hasBindingDirect(name);
        break;
      default:
        exists = TRY(env->HasBinding(name));
        break;
    }
    if (exists) {
      ref.kind = Reference::Kind::Environment;
      ref.env = env;
      return ref;
    }
  }
  ref.kind = Reference::Kind::Unresolvable;
  return ref;
}

namespace {

Throw throwNotDefined(String* name) { return ThrowReferenceError(name->toUtf8() + " is not defined"); }

// ResolveBinding + GetValue for an identifier, without materializing the Reference.
Result<Value> getIdentifierValue(String* name) {
  InterpContext* context = runningContext();
  for (Environment* env = context->lexicalEnvironment; env; env = env->outer) {
    switch (env->kind()) {
      case Environment::Kind::Declarative:
      case Environment::Kind::Function: {
        auto* binding = static_cast<DeclarativeEnvironment*>(env)->find(name);
        if (!binding) continue;
        if (binding->value.isEmpty()) {
          return ThrowReferenceError("Cannot access '" + name->toUtf8() + "' before initialization");
        }
        return binding->value;
      }
      case Environment::Kind::Module:
        if (!static_cast<DeclarativeEnvironment*>(env)->hasBindingDirect(name)) continue;
        return env->GetBindingValue(name, true);
      case Environment::Kind::Global: {
        auto* global = static_cast<GlobalEnvironment*>(env);
        if (global->declarativeRecord->hasBindingDirect(name)) return global->declarativeRecord->GetBindingValue(name, true);
        if (std::optional<PropertySlot> slot = global->objectRecord->ownDataSlot(name)) return slot->value;
        bool exists = TRY(global->objectRecord->HasBinding(name));
        if (!exists) continue;
        return global->objectRecord->GetBindingValue(name, context->strict);
      }
      case Environment::Kind::Object: {
        bool exists = TRY(env->HasBinding(name));
        if (!exists) continue;
        return env->GetBindingValue(name, context->strict);
      }
    }
  }
  return throwNotDefined(name);
}

}  // namespace

Result<Value> ResolveThisBinding() {
  Environment* env = GetThisEnvironment(runningContext()->lexicalEnvironment);
  switch (env->kind()) {
    case Environment::Kind::Function:
      return static_cast<FunctionEnvironment*>(env)->GetThisBinding();
    case Environment::Kind::Global:
      return static_cast<GlobalEnvironment*>(env)->GetThisBinding();
    default:
      return Value::undefined();  // module code
  }
}

Value GetNewTarget() {
  Environment* env = GetThisEnvironment(runningContext()->lexicalEnvironment);
  if (env->kind() != Environment::Kind::Function) return Value::undefined();
  return static_cast<FunctionEnvironment*>(env)->newTarget;
}

// ---- References ----

Value GetThisValue(const Reference& ref) { return ref.isSuperReference() ? ref.thisValue : ref.base; }

Result<PropertyKey> referenceKey(Reference& ref) {
  if (!ref.key) ref.key = TRY(ToPropertyKey(ref.propertyName));
  return *ref.key;
}

namespace {

Throw throwNullishBase(Value base, Reference& ref, const char* verb) {
  std::string what = base.isUndefined() ? "undefined" : "null";
  std::string name;
  if (ref.key) {
    Value key = ref.key->toValue();
    if (key.isString()) name = key.asString()->toUtf8();
  } else if (ref.propertyName.isString()) {
    name = ref.propertyName.asString()->toUtf8();
  }
  if (name.empty()) return ThrowTypeError(std::string("Cannot ") + verb + " properties of " + what);
  return ThrowTypeError(std::string("Cannot ") + verb + " properties of " + what + " (" + verb + "ing '" + name + "')");
}

}  // namespace

Result<Value> GetValue(Reference& ref) {
  switch (ref.kind) {
    case Reference::Kind::Unresolvable:
      if (ref.name) return throwNotDefined(ref.name);
      return ref.base;  // a value (not a Reference)
    case Reference::Kind::Environment:
      return ref.env->GetBindingValue(ref.name, ref.strict);
    case Reference::Kind::Property: {
      if (ref.base.isNullish()) return throwNullishBase(ref.base, ref, "read");
      PropertyKey key = TRY(referenceKey(ref));
      if (ref.base.isObject()) return ref.base.asObject()->Get(key, GetThisValue(ref));
      Object* baseObj = TRY(ToObject(ref.base));
      return baseObj->Get(key, GetThisValue(ref));
    }
    case Reference::Kind::Private: {
      Object* baseObj = TRY(ToObject(ref.base));
      return PrivateGet(baseObj, ref.privateName);
    }
  }
  return Value::undefined();
}

Result<void> PutValue(Reference& ref, Value value) {
  switch (ref.kind) {
    case Reference::Kind::Unresolvable: {
      if (!ref.name) return ThrowReferenceError("Invalid assignment target");
      if (ref.strict) return throwNotDefined(ref.name);
      return Set(GetGlobalObject(), PropertyKey(ref.name), value, false);
    }
    case Reference::Kind::Environment:
      return ref.env->SetMutableBinding(ref.name, value, ref.strict);
    case Reference::Kind::Property: {
      if (ref.base.isNullish()) return throwNullishBase(ref.base, ref, "set");
      Object* baseObj = TRY(ToObject(ref.base));
      PropertyKey key = TRY(referenceKey(ref));
      bool succeeded = TRY(baseObj->Set(key, value, GetThisValue(ref)));
      if (!succeeded && ref.strict) {
        Value keyValue = key.toValue();
        std::string name = keyValue.isString() ? keyValue.asString()->toUtf8() : "symbol";
        return ThrowTypeError("Cannot assign to read only property '" + name + "'");
      }
      return {};
    }
    case Reference::Kind::Private: {
      Object* baseObj = TRY(ToObject(ref.base));
      return PrivateSet(baseObj, ref.privateName, value);
    }
  }
  return {};
}

Result<void> InitializeReferencedBinding(Reference& ref, Value value) {
  return ref.env->InitializeBinding(ref.name, value);
}

namespace {

Reference valueReference(Value value) {
  Reference ref;
  ref.kind = Reference::Kind::Unresolvable;
  ref.base = value;
  return ref;
}

bool strictCode() { return runningContext()->strict; }

// MakeSuperPropertyReference(actualThis, propertyKey, strict)
Result<Reference> makeSuperPropertyReference(Value actualThis, Value propertyName, std::optional<PropertyKey> key) {
  Environment* env = GetThisEnvironment(runningContext()->lexicalEnvironment);
  Value baseValue = TRY(static_cast<FunctionEnvironment*>(env)->GetSuperBase());
  Reference ref;
  ref.kind = Reference::Kind::Property;
  ref.base = baseValue;
  ref.propertyName = propertyName;
  ref.key = key;
  ref.thisValue = actualThis;
  ref.strict = strictCode();
  return ref;
}

Result<Reference> evaluateChainReference(const Node* node, bool* shortCircuit);

// The base of a member / call link: part of the same optional chain, or an ordinary expression.
Result<Reference> evaluateChainBase(const Node* node, bool* shortCircuit) {
  if (shortCircuit && (node->type == NodeType::MemberExpression || node->type == NodeType::CallExpression)) {
    return evaluateChainReference(node, shortCircuit);
  }
  return evaluateReference(node);
}

Result<Reference> evaluateMember(const MemberExpression* node, bool* shortCircuit) {
  if (node->object->type == NodeType::Super) {
    Environment* env = GetThisEnvironment(runningContext()->lexicalEnvironment);
    Value actualThis = TRY(static_cast<FunctionEnvironment*>(env)->GetThisBinding());
    if (!node->computed) {
      return makeSuperPropertyReference(actualThis, Value(), PropertyKey(identifierName(node->property->as<Identifier>())));
    }
    Value propertyNameValue = TRY(evaluate(node->property));
    return makeSuperPropertyReference(actualThis, propertyNameValue, std::nullopt);
  }
  Reference baseRef = TRY(evaluateChainBase(node->object, shortCircuit));
  if (shortCircuit && *shortCircuit) return baseRef;
  Value baseValue = TRY(GetValue(baseRef));
  if (node->optional && baseValue.isNullish()) {
    *shortCircuit = true;
    return valueReference(Value::undefined());
  }
  Reference ref;
  ref.base = baseValue;
  ref.strict = strictCode();
  if (node->property->type == NodeType::PrivateIdentifier) {
    ref.kind = Reference::Kind::Private;
    String* name = atomFor(node->property->as<PrivateIdentifier>()->name);
    ref.privateName = runningContext()->privateEnvironment->resolve(name);
    return ref;
  }
  ref.kind = Reference::Kind::Property;
  if (!node->computed) {
    ref.key = PropertyKey(identifierName(node->property->as<Identifier>()));
  } else {
    ref.propertyName = TRY(evaluate(node->property));
  }
  return ref;
}

// Evaluation + GetValue of `base.name` / `base[expression]` without materializing the Reference.
Result<Value> evaluatePropertyValue(const MemberExpression* node) {
  Value baseValue = TRY(evaluate(node->object));
  Value propertyName;
  std::optional<PropertyKey> key;
  if (!node->computed) {
    key = PropertyKey(identifierName(node->property->as<Identifier>()));
  } else {
    propertyName = TRY(evaluate(node->property));
  }
  if (baseValue.isNullish()) {
    Reference ref;
    ref.kind = Reference::Kind::Property;
    ref.base = baseValue;
    ref.propertyName = propertyName;
    ref.key = key;
    return throwNullishBase(baseValue, ref, "read");
  }
  if (!key) key = TRY(ToPropertyKey(propertyName));
  if (baseValue.isObject()) return baseValue.asObject()->Get(*key, baseValue);
  Object* baseObj = TRY(ToObject(baseValue));
  return baseObj->Get(*key, baseValue);
}

Value thisValueForCall(const Reference& ref) {
  if (ref.isPropertyReference()) return GetThisValue(ref);
  if (ref.kind == Reference::Kind::Environment) return ref.env->WithBaseObject();
  return Value::undefined();
}

Result<Value> evaluateSuperCall(const CallExpression* node) {
  Value newTarget = GetNewTarget();
  auto* thisEnv = static_cast<FunctionEnvironment*>(GetThisEnvironment(runningContext()->lexicalEnvironment));
  Object* activeFunction = thisEnv->functionObject;
  Object* func = activeFunction->prototype();  // ! activeFunction.[[GetPrototypeOf]]()
  ValueVector argList = TRY(ArgumentListEvaluation(node->arguments));
  if (!func || !func->isConstructor()) return ThrowTypeError("Super constructor is not a constructor");
  Object* result = TRY(Construct(func, argList, newTarget.asObject()));
  TRY(thisEnv->BindThisValue(Value(result)));
  TRY(InitializeInstanceElements(result, static_cast<ECMAScriptFunction*>(thisEnv->functionObject)));
  return Value(result);
}

Result<Reference> evaluateCallLink(const CallExpression* node, bool* shortCircuit) {
  if (node->callee->type == NodeType::Super) return valueReference(TRY(evaluateSuperCall(node)));
  Reference ref = TRY(evaluateChainBase(node->callee, shortCircuit));
  if (shortCircuit && *shortCircuit) return ref;
  Value func = TRY(GetValue(ref));
  if (node->optional && func.isNullish()) {
    *shortCircuit = true;
    return valueReference(Value::undefined());
  }
  if (node->isDirectEvalCandidate && ref.kind == Reference::Kind::Environment && ref.name == atom(u"eval") &&
      func.isObject() && func.asObject() == vm().currentRealm()->intrinsic(Intrinsic::eval)) {
    ValueVector argList = TRY(ArgumentListEvaluation(node->arguments));
    if (argList.empty()) return valueReference(Value::undefined());
    return valueReference(TRY(PerformEval(argList[0], strictCode(), true)));
  }
  return valueReference(TRY(EvaluateCall(func, thisValueForCall(ref), node->arguments)));
}

Result<Reference> evaluateChainReference(const Node* node, bool* shortCircuit) {
  if (node->type == NodeType::MemberExpression) return evaluateMember(node->as<MemberExpression>(), shortCircuit);
  return evaluateCallLink(node->as<CallExpression>(), shortCircuit);
}

}  // namespace

Result<Reference> evaluateReference(const Node* node) {
  switch (node->type) {
    case NodeType::Identifier:
      return ResolveBinding(identifierName(node->as<Identifier>()));
    case NodeType::MemberExpression:
      return evaluateMember(node->as<MemberExpression>(), nullptr);
    case NodeType::CallExpression:
      return evaluateCallLink(node->as<CallExpression>(), nullptr);
    case NodeType::ChainExpression: {
      bool shortCircuit = false;
      Reference ref = TRY(evaluateChainReference(node->as<ChainExpression>()->expression, &shortCircuit));
      if (shortCircuit) return valueReference(Value::undefined());
      return ref;
    }
    default:
      return valueReference(TRY(evaluate(node)));
  }
}

// ---- Function definitions ----

bool IsAnonymousFunctionDefinition(const Node* node) {
  switch (node->type) {
    case NodeType::FunctionExpression:
      return node->as<FunctionExpression>()->id == nullptr;
    case NodeType::ArrowFunctionExpression:
      return true;
    case NodeType::ClassExpression:
      return node->as<ClassExpression>()->id == nullptr;
    default:
      return false;
  }
}

Result<Value> NamedEvaluation(const Node* node, Value name) {
  if (node->type == NodeType::ClassExpression) return evaluateClass(node->as<ClassExpression>(), name);
  return InstantiateFunctionExpression(node->as<Function>(), name);
}

Result<Value> evaluateNamed(const Node* node, Value name) {
  if (IsAnonymousFunctionDefinition(node)) return NamedEvaluation(node, name);
  return evaluate(node);
}

// ---- Calls ----

Result<ValueVector> ArgumentListEvaluation(const std::vector<Node*>& arguments) {
  ValueVector list;
  list.reserve(arguments.size());
  for (const Node* argument : arguments) {
    if (argument->type == NodeType::SpreadElement) {
      Value spreadObj = TRY(evaluate(argument->as<SpreadElement>()->argument));
      IteratorRecord record = TRY(GetIterator(spreadObj, IteratorKind::Sync));
      while (true) {
        std::optional<Value> next = TRY(IteratorStepValue(record));
        if (!next) break;
        list.push_back(*next);
      }
      continue;
    }
    list.push_back(TRY(evaluate(argument)));
  }
  return list;
}

Result<Value> EvaluateCall(Value func, Value thisValue, const std::vector<Node*>& arguments) {
  ValueVector argList = TRY(ArgumentListEvaluation(arguments));
  if (!func.isObject() || !func.asObject()->isCallable()) return ThrowTypeError("not a function");
  return func.asObject()->Call(thisValue, argList);
}

// ---- Literals, templates, property names ----

namespace {

Result<Value> evaluateLiteral(const Literal* node) {
  switch (node->kind) {
    case LiteralKind::Null:
      return Value::null();
    case LiteralKind::Boolean:
      return Value::boolean(node->boolean);
    case LiteralKind::Number:
      return Value::number(node->number);
    case LiteralKind::String:
      if (!node->interpCache) node->interpCache = atom(node->string);
      return Value(static_cast<String*>(node->interpCache));
    case LiteralKind::BigInt:
      return Value(BigInt::fromDigits(node->bigint, 10));
    case LiteralKind::RegExp: {
      Object* regexpConstructor = vm().currentRealm()->intrinsic(Intrinsic::RegExp);
      if (!regexpConstructor) return ThrowSyntaxError("RegExp is not supported");
      Value pattern(newString(node->regex.pattern));
      Value flags(newString(node->regex.flags));
      Object* regexp = TRY(Construct(regexpConstructor, {pattern, flags}));
      return Value(regexp);
    }
  }
  return Value::undefined();
}

Result<Value> evaluateTemplateLiteral(const TemplateLiteral* node) {
  std::u16string result = *node->quasis[0]->cooked;
  for (size_t i = 0; i < node->expressions.size(); i++) {
    Value sub = TRY(evaluate(node->expressions[i]));
    String* middle = TRY(ToString(sub));
    result += middle->view();
    result += *node->quasis[i + 1]->cooked;
  }
  return Value(newString(std::move(result)));
}

}  // namespace

Result<Value> GetTemplateObject(const TemplateLiteral* templateLiteral) {
  Realm* realm = vm().currentRealm();
  RealmState* state = realmState(realm);
  if (templateLiteral->interpCache) return Value(static_cast<Object*>(templateLiteral->interpCache));
  size_t count = templateLiteral->quasis.size();
  Object* templateObject = TRY(ArrayCreate(double(count)));
  Object* rawObj = TRY(ArrayCreate(double(count)));
  for (size_t index = 0; index < count; index++) {
    const TemplateElement* element = templateLiteral->quasis[index];
    PropertyKey prop = PropertyKey::index(uint32_t(index));
    Value cookedValue = element->cooked ? Value(newString(*element->cooked)) : Value::undefined();
    TRY(DefinePropertyOrThrow(templateObject, prop, PropertyDescriptor::data(cookedValue, Attr::Enumerable)));
    TRY(DefinePropertyOrThrow(rawObj, prop, PropertyDescriptor::data(Value(newString(element->raw)), Attr::Enumerable)));
  }
  TRY(SetIntegrityLevel(rawObj, IntegrityLevel::Frozen));
  TRY(DefinePropertyOrThrow(templateObject, PropertyKey(u"raw"), PropertyDescriptor::data(Value(rawObj), Attr::None)));
  TRY(SetIntegrityLevel(templateObject, IntegrityLevel::Frozen));
  state->templateMap.emplace_back(templateLiteral, templateObject);
  templateLiteral->interpCache = templateObject;
  return Value(templateObject);
}

Result<Value> evaluatePropertyKey(const Node* key, bool computed) {
  if (computed) {
    Value value = TRY(evaluate(key));
    PropertyKey propertyKey = TRY(ToPropertyKey(value));
    return propertyKey.toValue();
  }
  if (key->type == NodeType::Identifier) return Value(identifierName(key->as<Identifier>()));
  const Literal* literal = key->as<Literal>();
  switch (literal->kind) {
    case LiteralKind::String:
      return Value(atom(literal->string));
    case LiteralKind::Number:
      return Value(NumberToString(literal->number));
    case LiteralKind::BigInt:
      return Value(newString(BigInt::fromDigits(literal->bigint, 10)->toString()));
    default:
      return Value(TRY(ToString(TRY(evaluateLiteral(literal)))));
  }
}

namespace {

// ---- Array and object literals ----

Result<Value> evaluateArray(const ArrayExpression* node) {
  Object* array = TRY(ArrayCreate(0));
  uint32_t nextIndex = 0;
  for (const Node* element : node->elements) {
    if (!element) {
      nextIndex++;
      TRY(Set(array, PropertyKey(u"length"), Value::number(nextIndex), true));
      continue;
    }
    if (element->type == NodeType::SpreadElement) {
      Value spreadObj = TRY(evaluate(element->as<SpreadElement>()->argument));
      IteratorRecord record = TRY(GetIterator(spreadObj, IteratorKind::Sync));
      while (true) {
        std::optional<Value> next = TRY(IteratorStepValue(record));
        if (!next) break;
        TRY(CreateDataPropertyOrThrow(array, PropertyKey::index(nextIndex), *next));
        nextIndex++;
      }
      continue;
    }
    Value initValue = TRY(evaluate(element));
    TRY(CreateDataPropertyOrThrow(array, PropertyKey::index(nextIndex), initValue));
    nextIndex++;
  }
  return Value(array);
}

bool isProtoKey(const Node* key) {
  if (key->type == NodeType::Identifier) return key->as<Identifier>()->name == u"__proto__";
  if (key->type == NodeType::Literal) {
    const Literal* literal = key->as<Literal>();
    return literal->kind == LiteralKind::String && literal->string == u"__proto__";
  }
  return false;
}

Result<void> definePropertyFromLiteral(Object* object, const Property* property) {
  if (property->kind == PropertyKind::Init && !property->method) {
    if (!property->computed && !property->shorthand && isProtoKey(property->key)) {
      Value propValue = TRY(evaluate(property->value));
      if (propValue.isObject() || propValue.isNull()) {
        TRY(object->SetPrototypeOf(propValue.isNull() ? nullptr : propValue.asObject()));
      }
      return {};
    }
    Value propKey = TRY(evaluatePropertyKey(property->key, property->computed));
    Value propValue = TRY(evaluateNamed(property->value, propKey));
    return CreateDataPropertyOrThrow(object, TRY(ToPropertyKey(propKey)), propValue);
  }
  Value propKey = TRY(evaluatePropertyKey(property->key, property->computed));
  const Function* function = property->value->as<Function>();
  ECMAScriptFunction* closure = TRY(DefineMethod(function, object));
  if (property->kind == PropertyKind::Init) {
    SetFunctionName(closure, propKey);
    return DefineMethodProperty(object, propKey, closure, true);
  }
  bool getter = property->kind == PropertyKind::Get;
  SetFunctionName(closure, propKey, getter ? u"get" : u"set");
  PropertyDescriptor desc;
  if (getter) {
    desc.get = Value(closure);
  } else {
    desc.set = Value(closure);
  }
  desc.enumerable = true;
  desc.configurable = true;
  return DefinePropertyOrThrow(object, TRY(ToPropertyKey(propKey)), desc);
}

Result<Value> evaluateObject(const ObjectExpression* node) {
  Object* object = OrdinaryObjectCreate(vm().currentRealm()->intrinsic(Intrinsic::ObjectPrototype));
  for (const Node* member : node->properties) {
    if (member->type == NodeType::SpreadElement) {
      Value fromValue = TRY(evaluate(member->as<SpreadElement>()->argument));
      TRY(CopyDataProperties(object, fromValue, {}));
      continue;
    }
    TRY(definePropertyFromLiteral(object, member->as<Property>()));
  }
  return Value(object);
}

// ---- Operators ----

Value typeofValue(Value value) {
  const char16_t* name = u"undefined";
  switch (value.type()) {
    case ValueType::Undefined:
    case ValueType::Empty:
      name = u"undefined";
      break;
    case ValueType::Null:
      name = u"object";
      break;
    case ValueType::Boolean:
      name = u"boolean";
      break;
    case ValueType::Number:
      name = u"number";
      break;
    case ValueType::String:
      name = u"string";
      break;
    case ValueType::Symbol:
      name = u"symbol";
      break;
    case ValueType::BigInt:
      name = u"bigint";
      break;
    case ValueType::Object:
      name = value.asObject()->isCallable() ? u"function" : u"object";
      break;
  }
  return Value(atom(name));
}

Result<Value> evaluateDelete(const UnaryExpression* node) {
  const Node* argument = node->argument;
  if (argument->type == NodeType::Identifier) {
    Reference ref = TRY(ResolveBinding(identifierName(argument->as<Identifier>())));
    if (ref.kind == Reference::Kind::Unresolvable) return Value::boolean(true);
    return Value::boolean(TRY(ref.env->DeleteBinding(ref.name)));
  }
  if (argument->type != NodeType::MemberExpression && argument->type != NodeType::ChainExpression) {
    TRY(evaluate(argument));
    return Value::boolean(true);
  }
  Reference ref = TRY(evaluateReference(argument));
  if (ref.kind != Reference::Kind::Property) return Value::boolean(true);  // a short-circuited chain
  if (ref.isSuperReference()) return ThrowReferenceError("Unsupported reference to 'super'");
  Object* baseObj = TRY(ToObject(ref.base));
  PropertyKey key = TRY(referenceKey(ref));
  bool deleteStatus = TRY(baseObj->Delete(key));
  if (!deleteStatus && ref.strict) return ThrowTypeError("Cannot delete property");
  return Value::boolean(deleteStatus);
}

Result<Value> evaluateUnary(const UnaryExpression* node) {
  switch (node->op) {
    case UnaryOperator::Delete:
      return evaluateDelete(node);
    case UnaryOperator::Void:
      TRY(evaluate(node->argument));
      return Value::undefined();
    case UnaryOperator::Typeof: {
      if (node->argument->type == NodeType::Identifier) {
        Reference ref = TRY(ResolveBinding(identifierName(node->argument->as<Identifier>())));
        if (ref.kind == Reference::Kind::Unresolvable) return Value(atom(u"undefined"));
        return typeofValue(TRY(GetValue(ref)));
      }
      return typeofValue(TRY(evaluate(node->argument)));
    }
    case UnaryOperator::Not:
      return Value::boolean(!ToBoolean(TRY(evaluate(node->argument))));
    default:
      return evaluateUnaryNumeric(node->op, TRY(evaluate(node->argument)));
  }
}

// An assignment or update target's Reference. Annex B (sloppy code may target a call): the call is evaluated,
// then a ReferenceError is thrown before anything else happens.
Result<Reference> evaluateTargetReference(const Node* target) {
  if (target->type == NodeType::CallExpression) {
    TRY(evaluate(target));
    return ThrowReferenceError("Invalid assignment target");
  }
  return evaluateReference(target);
}

Result<Value> evaluateUpdate(const UpdateExpression* node) {
  Reference lhs = TRY(evaluateTargetReference(node->argument));
  Value oldValue = TRY(ToNumeric(TRY(GetValue(lhs))));
  Value newValue;
  bool increment = node->op == UpdateOperator::Increment;
  if (oldValue.isNumber()) {
    newValue = Value::number(oldValue.asNumber() + (increment ? 1 : -1));
  } else {
    BigInt* one = BigInt::fromInt64(1);
    newValue = Value(increment ? BigInt::add(oldValue.asBigInt(), one) : BigInt::subtract(oldValue.asBigInt(), one));
  }
  TRY(PutValue(lhs, newValue));
  return node->prefix ? newValue : oldValue;
}

Result<Value> evaluateBinary(const BinaryExpression* node) {
  if (node->left->type == NodeType::PrivateIdentifier) {
    Value rval = TRY(evaluate(node->right));
    if (!rval.isObject()) return ThrowTypeError("Cannot use 'in' operator to search for a private field in a non-object");
    String* name = atomFor(node->left->as<PrivateIdentifier>()->name);
    PrivateName* privateName = runningContext()->privateEnvironment->resolve(name);
    return Value::boolean(PrivateElementFind(rval.asObject(), privateName) != nullptr);
  }
  Value lval = TRY(evaluate(node->left));
  Value rval = TRY(evaluate(node->right));
  return evaluateBinaryOperator(node->op, lval, rval);
}

Result<Value> evaluateLogical(const LogicalExpression* node) {
  Value lval = TRY(evaluate(node->left));
  switch (node->op) {
    case LogicalOperator::And:
      if (!ToBoolean(lval)) return lval;
      break;
    case LogicalOperator::Or:
      if (ToBoolean(lval)) return lval;
      break;
    case LogicalOperator::Coalesce:
      if (!lval.isNullish()) return lval;
      break;
  }
  return evaluate(node->right);
}

bool isIdentifierRef(const Node* node) { return node->type == NodeType::Identifier && !node->parenthesized; }

Result<Value> evaluateAssignment(const AssignmentExpression* node) {
  const Node* left = node->left;
  if (node->op == AssignmentOperator::Assign) {
    if (left->type == NodeType::ObjectPattern || left->type == NodeType::ArrayPattern) {
      Value rval = TRY(evaluate(node->right));
      TRY(DestructuringAssignmentEvaluation(left, rval));
      return rval;
    }
    Reference lref = TRY(evaluateTargetReference(left));
    Value rval;
    if (isIdentifierRef(left) && IsAnonymousFunctionDefinition(node->right)) {
      rval = TRY(NamedEvaluation(node->right, Value(lref.name)));
    } else {
      rval = TRY(evaluate(node->right));
    }
    TRY(PutValue(lref, rval));
    return rval;
  }
  Reference lref = TRY(evaluateTargetReference(left));
  Value lval = TRY(GetValue(lref));
  if (node->op == AssignmentOperator::And || node->op == AssignmentOperator::Or ||
      node->op == AssignmentOperator::Coalesce) {
    bool shortCircuit = node->op == AssignmentOperator::And   ? !ToBoolean(lval)
                        : node->op == AssignmentOperator::Or ? ToBoolean(lval)
                                                              : !lval.isNullish();
    if (shortCircuit) return lval;
    Value rval;
    if (isIdentifierRef(left) && IsAnonymousFunctionDefinition(node->right)) {
      rval = TRY(NamedEvaluation(node->right, Value(lref.name)));
    } else {
      rval = TRY(evaluate(node->right));
    }
    TRY(PutValue(lref, rval));
    return rval;
  }
  Value rval = TRY(evaluate(node->right));
  Value r = TRY(ApplyStringOrNumericBinaryOperator(lval, binaryOperatorFor(node->op), rval));
  TRY(PutValue(lref, r));
  return r;
}

Result<Value> evaluateNew(const NewExpression* node) {
  Value constructor = TRY(evaluate(node->callee));
  ValueVector argList = TRY(ArgumentListEvaluation(node->arguments));
  if (!IsConstructor(constructor)) return ThrowTypeError("not a constructor");
  return Value(TRY(Construct(constructor.asObject(), argList)));
}

Result<Value> evaluateTaggedTemplate(const TaggedTemplateExpression* node) {
  Reference tagRef = TRY(evaluateReference(node->tag));
  Value tagFunc = TRY(GetValue(tagRef));
  Value thisValue = thisValueForCall(tagRef);
  ValueVector argList;
  argList.push_back(TRY(GetTemplateObject(node->quasi)));
  for (const Node* expression : node->quasi->expressions) argList.push_back(TRY(evaluate(expression)));
  if (!tagFunc.isObject() || !tagFunc.asObject()->isCallable()) return ThrowTypeError("not a function");
  return tagFunc.asObject()->Call(thisValue, argList);
}

Result<Value> evaluateMetaProperty(const MetaProperty* node) {
  if (node->meta->name == u"new") return GetNewTarget();
  return evaluateImportMeta();
}

}  // namespace

// ---- The dispatcher ----

Result<Value> evaluate(const Node* node) {
  switch (node->type) {
    case NodeType::Identifier:
      return getIdentifierValue(identifierName(node->as<Identifier>()));
    case NodeType::Literal:
      return evaluateLiteral(node->as<Literal>());
    case NodeType::ThisExpression:
      return ResolveThisBinding();
    case NodeType::TemplateLiteral:
      return evaluateTemplateLiteral(node->as<TemplateLiteral>());
    case NodeType::ArrayExpression:
      return evaluateArray(node->as<ArrayExpression>());
    case NodeType::ObjectExpression:
      return evaluateObject(node->as<ObjectExpression>());
    case NodeType::FunctionExpression:
    case NodeType::ArrowFunctionExpression:
      return InstantiateFunctionExpression(node->as<Function>(), Value::empty());
    case NodeType::ClassExpression:
      return evaluateClass(node->as<ClassExpression>(), Value::empty());
    case NodeType::UnaryExpression:
      return evaluateUnary(node->as<UnaryExpression>());
    case NodeType::UpdateExpression:
      return evaluateUpdate(node->as<UpdateExpression>());
    case NodeType::BinaryExpression:
      return evaluateBinary(node->as<BinaryExpression>());
    case NodeType::LogicalExpression:
      return evaluateLogical(node->as<LogicalExpression>());
    case NodeType::AssignmentExpression:
      return evaluateAssignment(node->as<AssignmentExpression>());
    case NodeType::ConditionalExpression: {
      auto* conditional = node->as<ConditionalExpression>();
      Value test = TRY(evaluate(conditional->test));
      return evaluate(ToBoolean(test) ? conditional->consequent : conditional->alternate);
    }
    case NodeType::MemberExpression: {
      TRY(CheckStack());
      auto* member = node->as<MemberExpression>();
      if (member->object->type != NodeType::Super && member->property->type != NodeType::PrivateIdentifier &&
          !member->optional) {
        return evaluatePropertyValue(member);
      }
      Reference ref = TRY(evaluateReference(node));
      return GetValue(ref);
    }
    case NodeType::CallExpression:
    case NodeType::ChainExpression: {
      TRY(CheckStack());
      Reference ref = TRY(evaluateReference(node));
      return GetValue(ref);
    }
    case NodeType::NewExpression:
      return evaluateNew(node->as<NewExpression>());
    case NodeType::SequenceExpression: {
      Value value;
      for (const Node* expression : node->as<SequenceExpression>()->expressions) value = TRY(evaluate(expression));
      return value;
    }
    case NodeType::TaggedTemplateExpression:
      return evaluateTaggedTemplate(node->as<TaggedTemplateExpression>());
    case NodeType::MetaProperty:
      return evaluateMetaProperty(node->as<MetaProperty>());
    case NodeType::YieldExpression: {
      auto* yield = node->as<YieldExpression>();
      return yield->delegate ? evaluateYieldStar(yield) : evaluateYield(yield);
    }
    case NodeType::AwaitExpression: {
      Value value = TRY(evaluate(node->as<AwaitExpression>()->argument));
      return Await(value);
    }
    case NodeType::ImportExpression:
      return evaluateImportCall(node->as<ImportExpression>());
    default:
      return ThrowSyntaxError(std::string("Unsupported expression: ") + nodeTypeName(node->type));
  }
}

}  // namespace snowman::interp
