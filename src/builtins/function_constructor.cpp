// The Function (20.2.1), GeneratorFunction (27.3), AsyncGeneratorFunction (27.4) and AsyncFunction (27.7)
// constructors, CreateDynamicFunction (20.2.1.1.1), and their prototype objects.
#include "interp/interpreter.h"
#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/symbol.h"

namespace snowman::interp {

namespace {

using parser::DynamicFunctionKind;

// 20.2.1.1.1 CreateDynamicFunction(constructor, newTarget, kind, parameterArgs, bodyArg)
Result<Value> CreateDynamicFunction(Object* constructor, Object* newTarget, DynamicFunctionKind kind, Args args) {
  if (!newTarget) newTarget = constructor;
  Intrinsic fallbackProto = Intrinsic::FunctionPrototype;
  switch (kind) {
    case DynamicFunctionKind::Normal:
      break;
    case DynamicFunctionKind::Generator:
      fallbackProto = Intrinsic::GeneratorFunctionPrototype;
      break;
    case DynamicFunctionKind::Async:
      fallbackProto = Intrinsic::AsyncFunctionPrototype;
      break;
    case DynamicFunctionKind::AsyncGenerator:
      fallbackProto = Intrinsic::AsyncGeneratorFunctionPrototype;
      break;
  }
  size_t argCount = args.size();
  std::u16string P;
  Value bodyArg = Value::undefined();
  if (argCount > 0) bodyArg = args[argCount - 1];
  for (size_t k = 0; k + 1 < argCount; k++) {
    String* parameter = TRY(ToString(args[k]));
    if (k > 0) P += u",";
    P += parameter->view();
  }
  std::u16string bodyString;
  if (argCount > 0) bodyString = TRY(ToString(bodyArg))->view();
  parser::ParseResult parsed = parser::parseDynamicFunction(kind, std::move(P), std::move(bodyString));
  if (!parsed.ok()) return throwParseError(*parsed.error);
  Object* proto = TRY(GetPrototypeFromConstructor(newTarget, fallbackProto));
  Realm* realm = vm().currentRealm();
  ast::FunctionExpression* function = parsed.function;
  ECMAScriptFunction* F = OrdinaryFunctionCreate(proto, function, false, globalEnvironment(realm), nullptr);
  SetFunctionName(F, Value(atom(u"anonymous")));
  if (kind == DynamicFunctionKind::Generator || kind == DynamicFunctionKind::AsyncGenerator) {
    Intrinsic prototypeId =
        kind == DynamicFunctionKind::Generator ? Intrinsic::GeneratorPrototype : Intrinsic::AsyncGeneratorPrototype;
    Object* prototype = OrdinaryObjectCreate(realm->intrinsic(prototypeId));
    defineValue(F, PropertyKey(u"prototype"), Value(prototype), Attr::Writable);
  } else if (kind == DynamicFunctionKind::Normal) {
    AddLegacyFunctionProperties(F);
    MakeConstructor(F);
  }
  return Value(F);
}

Result<Value> functionConstructor(NativeCall& call) {
  return CreateDynamicFunction(call.callee, call.newTarget, DynamicFunctionKind::Normal, call.args);
}
Result<Value> generatorFunctionConstructor(NativeCall& call) {
  return CreateDynamicFunction(call.callee, call.newTarget, DynamicFunctionKind::Generator, call.args);
}
Result<Value> asyncFunctionConstructor(NativeCall& call) {
  return CreateDynamicFunction(call.callee, call.newTarget, DynamicFunctionKind::Async, call.args);
}
Result<Value> asyncGeneratorFunctionConstructor(NativeCall& call) {
  return CreateDynamicFunction(call.callee, call.newTarget, DynamicFunctionKind::AsyncGenerator, call.args);
}

// %XFunction% and %XFunction.prototype%: the constructor inherits from %Function%, the prototype from
// %Function.prototype%; instancePrototype (if any) becomes %XFunction.prototype.prototype%.
void installFunctionKind(Realm& realm, NativeFn fn, const char16_t* name, Intrinsic constructorId,
                         Intrinsic prototypeId, Intrinsic instancePrototypeId) {
  Object* functionConstructor = realm.intrinsic(Intrinsic::Function);
  BuiltinFunction* constructor =
      createBuiltinFunction(realm, fn, 1, PropertyKey(name), true, functionConstructor);
  Object* prototype = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::FunctionPrototype));
  defineValue(constructor, PropertyKey(u"prototype"), Value(prototype), Attr::None);
  defineValue(prototype, PropertyKey(u"constructor"), Value(constructor), Attr::Configurable);
  if (instancePrototypeId != Intrinsic::Count) {
    Object* instancePrototype = realm.intrinsic(instancePrototypeId);
    defineValue(prototype, PropertyKey(u"prototype"), Value(instancePrototype), Attr::Configurable);
    defineValue(instancePrototype, PropertyKey(u"constructor"), Value(prototype), Attr::Configurable);
  }
  defineValue(prototype, PropertyKey(vm().symbols.toStringTag), Value(atom(name)), Attr::Configurable);
  realm.setIntrinsic(constructorId, constructor);
  realm.setIntrinsic(prototypeId, prototype);
}

void installFunctionConstructors(Realm& realm) {
  Object* functionPrototype = realm.intrinsic(Intrinsic::FunctionPrototype);
  BuiltinFunction* function = createBuiltinFunction(realm, functionConstructor, 1, PropertyKey(u"Function"), true);
  defineValue(function, PropertyKey(u"prototype"), Value(functionPrototype), Attr::None);
  defineValue(functionPrototype, PropertyKey(u"constructor"), Value(function), Attr::WritableConfigurable);
  realm.setIntrinsic(Intrinsic::Function, function);
  defineGlobal(realm, PropertyKey(u"Function"), Value(function));

  installFunctionKind(realm, generatorFunctionConstructor, u"GeneratorFunction", Intrinsic::GeneratorFunction,
                      Intrinsic::GeneratorFunctionPrototype, Intrinsic::GeneratorPrototype);
  installFunctionKind(realm, asyncGeneratorFunctionConstructor, u"AsyncGeneratorFunction",
                      Intrinsic::AsyncGeneratorFunction, Intrinsic::AsyncGeneratorFunctionPrototype,
                      Intrinsic::AsyncGeneratorPrototype);
  installFunctionKind(realm, asyncFunctionConstructor, u"AsyncFunction", Intrinsic::AsyncFunction,
                      Intrinsic::AsyncFunctionPrototype, Intrinsic::Count);
}

}  // namespace

// After the generator prototypes (kOrderDefault), before anything that creates ECMAScript functions.
SNOWMAN_BUILTIN(FunctionConstructors, kOrderDefault + 1, installFunctionConstructors);

}  // namespace snowman::interp
