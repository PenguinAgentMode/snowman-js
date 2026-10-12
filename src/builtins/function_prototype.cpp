// Function.prototype (20.2.3) methods, %ThrowTypeError% (10.2.4.1) and the restricted "caller" and
// "arguments" accessors. The Function constructor itself is the interpreter's (function_constructor.cpp).
#include <cmath>

#include "builtins/util.h"
#include "runtime/bound_function.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

// 20.2.3.1 Function.prototype.apply ( thisArg, argArray )
Result<Value> FunctionPrototype_apply(NativeCall& call) {
  Value func = call.thisValue;
  if (!IsCallable(func)) return ThrowTypeError("Function.prototype.apply was called on a non-function");
  Value argArray = call.args[1];
  if (argArray.isNullish()) return Call(func, call.args[0]);
  ValueVector argList = TRY(CreateListFromArrayLike(argArray));
  return Call(func, call.args[0], argList);
}

// 20.2.3.2 Function.prototype.bind ( thisArg, ...args )
Result<Value> FunctionPrototype_bind(NativeCall& call) {
  Value target = call.thisValue;
  if (!IsCallable(target)) return ThrowTypeError("Bind must be called on a function");
  Args args = call.args.slice(1);
  BoundFunction* f = TRY(BoundFunctionCreate(target.asObject(), call.args[0], args));
  double l = 0;
  bool targetHasLength = TRY(HasOwnProperty(target.asObject(), PropertyKey(u"length")));
  if (targetHasLength) {
    Value targetLen = TRY(Get(target.asObject(), PropertyKey(u"length")));
    if (targetLen.isNumber()) {
      double tl = targetLen.asNumber();
      if (tl == INFINITY) {
        l = INFINITY;
      } else if (tl == -INFINITY) {
        l = 0;
      } else {
        double targetLenAsInt = TRY(ToIntegerOrInfinity(targetLen));
        l = std::max(targetLenAsInt - double(args.size()), 0.0);
      }
    }
  }
  SetFunctionLength(f, l);
  Value targetName = TRY(Get(target.asObject(), PropertyKey(u"name")));
  if (!targetName.isString()) targetName = Value(emptyString());
  SetFunctionName(f, PropertyKey(targetName.asString()), u"bound");
  return Value(f);
}

// 20.2.3.3 Function.prototype.call ( thisArg, ...args )
Result<Value> FunctionPrototype_call(NativeCall& call) {
  Value func = call.thisValue;
  if (!IsCallable(func)) return ThrowTypeError("Function.prototype.call was called on a non-function");
  return Call(func, call.args[0], call.args.slice(1));
}

// 20.2.3.5 Function.prototype.toString ( )
Result<Value> FunctionPrototype_toString(NativeCall& call) {
  Value func = call.thisValue;
  if (func.isObject()) {
    if (auto* f = func.asObject()->as<FunctionObject>()) {
      if (String* source = f->sourceText()) return Value(source);
    }
    if (auto* b = func.asObject()->as<BuiltinFunction>()) {
      std::u16string_view name = b->initialName.isString() ? b->initialName.asString()->view() : u"";
      return Value(concatStrings({u"function ", name, u"() { [native code] }"}));
    }
    if (IsCallable(func)) return str(u"function () { [native code] }");
  }
  return ThrowTypeError("Function.prototype.toString requires that 'this' be a Function");
}

// 20.2.3.6 Function.prototype [ @@hasInstance ] ( V )
Result<Value> FunctionPrototype_hasInstance(NativeCall& call) {
  return Value::boolean(TRY(OrdinaryHasInstance(call.thisValue, call.args[0])));
}

// 10.2.4.1 %ThrowTypeError% ( )
Result<Value> ThrowTypeErrorFunction(NativeCall&) {
  return ThrowTypeError("'caller', 'callee', and 'arguments' properties may not be accessed on strict mode functions or the arguments objects for calls to them");
}

void installFunctionPrototype(Realm& realm) {
  Object* proto = realm.intrinsic(Intrinsic::FunctionPrototype);

  BuiltinFunction* thrower = createBuiltinFunction(realm, ThrowTypeErrorFunction, 0, PropertyKey(u""));
  thrower->properties().put(PropertyKey(u"length"), PropertySlot::data(Value::number(0), Attr::None));
  thrower->properties().put(PropertyKey(u"name"), PropertySlot::data(emptyString(), Attr::None));
  (void)thrower->PreventExtensions();
  realm.setIntrinsic(Intrinsic::ThrowTypeError, thrower);

  defineFunction(realm, proto, PropertyKey(u"apply"), 2, FunctionPrototype_apply);
  defineFunction(realm, proto, PropertyKey(u"bind"), 1, FunctionPrototype_bind);
  defineFunction(realm, proto, PropertyKey(u"call"), 1, FunctionPrototype_call);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, FunctionPrototype_toString);
  defineFunction(realm, proto, PropertyKey(vm().symbols.hasInstance), 1, FunctionPrototype_hasInstance, Attr::None);
  // 10.2.4 AddRestrictedFunctionProperties
  (void)proto->DefineOwnProperty(PropertyKey(u"caller"), PropertyDescriptor::accessor(thrower, thrower, Attr::Configurable));
  (void)proto->DefineOwnProperty(PropertyKey(u"arguments"), PropertyDescriptor::accessor(thrower, thrower, Attr::Configurable));
}

}  // namespace

SNOWMAN_BUILTIN(FunctionPrototype, kOrderFundamental, installFunctionPrototype);

}  // namespace snowman::builtins
