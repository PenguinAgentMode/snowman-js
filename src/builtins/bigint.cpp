// BigInt (21.2).
#include <cmath>

#include "builtins/util.h"
#include "runtime/bigint.h"
#include "runtime/wrappers.h"

namespace snowman::builtins {

namespace {

// 21.2.1.1.1 NumberToBigInt
Result<BigInt*> NumberToBigInt(double number) {
  if (!std::isfinite(number) || std::trunc(number) != number)
    return ThrowRangeError("The number cannot be converted to a BigInt because it is not an integer");
  return BigInt::fromDouble(number);
}

// 21.2.1.1 BigInt ( value )
Result<Value> BigInt_construct(NativeCall& call) {
  if (call.newTarget) return ThrowTypeError("BigInt is not a constructor");
  Value prim = TRY(ToPrimitive(call.args[0], PreferredType::Number));
  if (prim.isNumber()) return Value(TRY(NumberToBigInt(prim.asNumber())));
  return Value(TRY(ToBigInt(prim)));
}

// 21.2.2.1 BigInt.asIntN ( bits, bigint ), 21.2.2.2 BigInt.asUintN ( bits, bigint )
Result<Value> BigInt_asIntN(NativeCall& call) {
  double bits = TRY(ToIndex(call.args[0]));
  BigInt* bigint = TRY(ToBigInt(call.args[1]));
  return Value(BigInt::asIntN(uint64_t(bits), bigint));
}

Result<Value> BigInt_asUintN(NativeCall& call) {
  double bits = TRY(ToIndex(call.args[0]));
  BigInt* bigint = TRY(ToBigInt(call.args[1]));
  return Value(BigInt::asUintN(uint64_t(bits), bigint));
}

// 21.2.3.4.1 ThisBigIntValue
Result<BigInt*> thisBigIntValue(Value value) {
  if (value.isBigInt()) return value.asBigInt();
  if (value.isObject()) {
    if (auto* b = value.asObject()->as<BigIntObject>()) return b->bigintData;
  }
  return ThrowTypeError("BigInt.prototype method called on incompatible receiver");
}

// 21.2.3.3 BigInt.prototype.toString ( [ radix ] )
Result<Value> BigIntPrototype_toString(NativeCall& call) {
  BigInt* x = TRY(thisBigIntValue(call.thisValue));
  double radix = 10;
  if (!call.args[0].isUndefined()) radix = TRY(ToIntegerOrInfinity(call.args[0]));
  if (radix < 2 || radix > 36) return ThrowRangeError("toString() radix must be between 2 and 36");
  return Value(newString(x->toString(int(radix))));
}

Result<Value> BigIntPrototype_toLocaleString(NativeCall& call) {
  BigInt* x = TRY(thisBigIntValue(call.thisValue));
  return Value(newString(x->toString(10)));
}

Result<Value> BigIntPrototype_valueOf(NativeCall& call) { return Value(TRY(thisBigIntValue(call.thisValue))); }

void installBigInt(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor =
      defineConstructor(realm, Intrinsic::BigInt, BigInt_construct, 1, PropertyKey(u"BigInt"), proto, Intrinsic::BigIntPrototype);
  defineFunction(realm, ctor, PropertyKey(u"asIntN"), 2, BigInt_asIntN);
  defineFunction(realm, ctor, PropertyKey(u"asUintN"), 2, BigInt_asUintN);
  defineFunction(realm, proto, PropertyKey(u"toLocaleString"), 0, BigIntPrototype_toLocaleString);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, BigIntPrototype_toString);
  defineFunction(realm, proto, PropertyKey(u"valueOf"), 0, BigIntPrototype_valueOf);
  defineValue(proto, PropertyKey(vm().symbols.toStringTag), str(u"BigInt"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(BigInt, kOrderDefault, installBigInt);

}  // namespace snowman::builtins
