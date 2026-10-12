// The Atomics object (25.4). One agent, so every operation is a plain read-modify-write and wait can only
// time out (nothing else can notify).
#include <time.h>

#include <cmath>
#include <cstring>

#include "builtins/util.h"
#include "runtime/arraybuffer.h"
#include "runtime/bigint.h"
#include "runtime/typedarray.h"

namespace snowman::builtins {

namespace {

bool isUnclampedInteger(ElementType t) {
  return t == ElementType::Int8 || t == ElementType::Uint8 || t == ElementType::Int16 || t == ElementType::Uint16 ||
         t == ElementType::Int32 || t == ElementType::Uint32;
}

// 25.4.3.1 ValidateIntegerTypedArray ( typedArray, waitable )
Result<TypedArrayObject*> ValidateIntegerTypedArray(Value typedArray, bool waitable) {
  TypedArrayObject* ta = TRY(ValidateTypedArray(typedArray));
  if (waitable) {
    if (ta->elementType != ElementType::Int32 && ta->elementType != ElementType::BigInt64)
      return ThrowTypeError("Atomics: typed array must be an Int32Array or BigInt64Array");
  } else if (!isUnclampedInteger(ta->elementType) && !IsBigIntElementType(ta->elementType)) {
    return ThrowTypeError("Atomics: typed array must be an integer typed array");
  }
  return ta;
}

// 25.4.3.2 ValidateAtomicAccess ( taRecord, requestIndex )
Result<size_t> ValidateAtomicAccess(TypedArrayObject* ta, Value requestIndex) {
  double length = TypedArrayLength(ta);
  double accessIndex = TRY(ToIndex(requestIndex));
  if (accessIndex >= length) return ThrowRangeError("Atomics: index out of range");
  return size_t(accessIndex) * ElementSize(ta->elementType) + size_t(ta->byteOffset);
}

// 25.4.3.5 RevalidateAtomicAccess ( typedArray, byteIndexInBuffer )
Result<void> RevalidateAtomicAccess(TypedArrayObject* ta, size_t byteIndex) {
  if (IsTypedArrayOutOfBounds(ta)) return ThrowTypeError("Atomics: typed array is detached or out of bounds");
  if (byteIndex >= ta->buffer->byteLength()) return ThrowRangeError("Atomics: index out of range");
  return {};
}

// ToBigInt for BigInt arrays, else ToIntegerOrInfinity as a Number.
Result<Value> toIntegerValue(TypedArrayObject* ta, Value value) {
  if (IsBigIntElementType(ta->elementType)) return Value(TRY(ToBigInt(value)));
  return num(TRY(ToIntegerOrInfinity(value)));
}

// The element's bit pattern for a converted value (mod 2^size).
uint64_t bitsOf(ElementType type, Value v) {
  if (IsBigIntElementType(type)) return v.asBigInt()->toUint64();
  return uint64_t(DoubleToUint32(v.asNumber()));
}

uint64_t readBits(ArrayBufferObject* buffer, size_t byteIndex, size_t size) {
  uint64_t bits = 0;
  std::memcpy(&bits, buffer->data() + byteIndex, size);
  return bits;
}

void writeBits(ArrayBufferObject* buffer, size_t byteIndex, size_t size, uint64_t bits) {
  std::memcpy(buffer->data() + byteIndex, &bits, size);
}

enum class Op { Add, And, Exchange, Or, Sub, Xor };

// 25.4.3.17 AtomicReadModifyWrite ( typedArray, index, value, op )
template <Op O>
Result<Value> readModifyWrite(NativeCall& call) {
  TypedArrayObject* ta = TRY(ValidateIntegerTypedArray(call.args[0], false));
  size_t byteIndex = TRY(ValidateAtomicAccess(ta, call.args[1]));
  Value v = TRY(toIntegerValue(ta, call.args[2]));
  TRY(RevalidateAtomicAccess(ta, byteIndex));
  size_t size = ElementSize(ta->elementType);
  Value old = GetValueFromBuffer(ta->buffer, byteIndex, ta->elementType);
  uint64_t a = readBits(ta->buffer, byteIndex, size), b = bitsOf(ta->elementType, v), r = 0;
  switch (O) {
    case Op::Add: r = a + b; break;
    case Op::And: r = a & b; break;
    case Op::Exchange: r = b; break;
    case Op::Or: r = a | b; break;
    case Op::Sub: r = a - b; break;
    case Op::Xor: r = a ^ b; break;
  }
  writeBits(ta->buffer, byteIndex, size, r);
  return old;
}

// 25.4.7 Atomics.compareExchange ( typedArray, index, expectedValue, replacementValue )
Result<Value> Atomics_compareExchange(NativeCall& call) {
  TypedArrayObject* ta = TRY(ValidateIntegerTypedArray(call.args[0], false));
  size_t byteIndex = TRY(ValidateAtomicAccess(ta, call.args[1]));
  Value expected = TRY(toIntegerValue(ta, call.args[2]));
  Value replacement = TRY(toIntegerValue(ta, call.args[3]));
  TRY(RevalidateAtomicAccess(ta, byteIndex));
  size_t size = ElementSize(ta->elementType);
  uint64_t mask = size == 8 ? ~uint64_t(0) : (uint64_t(1) << (8 * size)) - 1;
  Value old = GetValueFromBuffer(ta->buffer, byteIndex, ta->elementType);
  if ((readBits(ta->buffer, byteIndex, size) & mask) == (bitsOf(ta->elementType, expected) & mask))
    writeBits(ta->buffer, byteIndex, size, bitsOf(ta->elementType, replacement));
  return old;
}

// 25.4.9 Atomics.isLockFree ( size )
Result<Value> Atomics_isLockFree(NativeCall& call) {
  double n = TRY(ToIntegerOrInfinity(call.args[0]));
  return boolean(n == 1 || n == 2 || n == 4 || n == 8);
}

// 25.4.10 Atomics.load ( typedArray, index )
Result<Value> Atomics_load(NativeCall& call) {
  TypedArrayObject* ta = TRY(ValidateIntegerTypedArray(call.args[0], false));
  size_t byteIndex = TRY(ValidateAtomicAccess(ta, call.args[1]));
  TRY(RevalidateAtomicAccess(ta, byteIndex));
  return GetValueFromBuffer(ta->buffer, byteIndex, ta->elementType);
}

// 25.4.12 Atomics.store ( typedArray, index, value )
Result<Value> Atomics_store(NativeCall& call) {
  TypedArrayObject* ta = TRY(ValidateIntegerTypedArray(call.args[0], false));
  size_t byteIndex = TRY(ValidateAtomicAccess(ta, call.args[1]));
  Value v = TRY(toIntegerValue(ta, call.args[2]));
  TRY(RevalidateAtomicAccess(ta, byteIndex));
  SetValueInBuffer(ta->buffer, byteIndex, ta->elementType, v);
  return v;
}

// 25.4.3.14 DoWait ( mode, typedArray, index, value, timeout ), synchronous mode.
Result<Value> Atomics_wait(NativeCall& call) {
  TypedArrayObject* ta = TRY(ValidateIntegerTypedArray(call.args[0], true));
  if (!ta->buffer->isShared()) return ThrowTypeError("Atomics.wait requires a shared typed array");
  size_t byteIndex = TRY(ValidateAtomicAccess(ta, call.args[1]));
  Value v;
  if (ta->elementType == ElementType::BigInt64) v = Value(BigInt::fromInt64(TRY(ToBigInt64(call.args[2]))));
  else v = num(TRY(ToInt32(call.args[2])));
  double q = TRY(ToNumber(call.args[3]));
  double t = std::isnan(q) || q == INFINITY ? INFINITY : q == -INFINITY ? 0 : std::max(q, 0.0);
  Value w = GetValueFromBuffer(ta->buffer, byteIndex, ta->elementType);
  if (!SameValue(v, w) && !(v.isBigInt() && w.isBigInt() && BigInt::equals(v.asBigInt(), w.asBigInt())))
    return str(u"not-equal");
  // No other agent can notify: wait out the timeout.
  if (std::isfinite(t) && t > 0) {
    struct timespec ts{time_t(t / 1000), long(std::fmod(t, 1000) * 1e6)};
    nanosleep(&ts, nullptr);
  }
  return str(u"timed-out");
}

// 25.4.15 Atomics.notify ( typedArray, index, count )
Result<Value> Atomics_notify(NativeCall& call) {
  TypedArrayObject* ta = TRY(ValidateIntegerTypedArray(call.args[0], true));
  TRY(ValidateAtomicAccess(ta, call.args[1]));
  if (!call.args[2].isUndefined()) TRY(ToIntegerOrInfinity(call.args[2]));
  return num(0);
}

// Atomics.pause ( [ N ] )
Result<Value> Atomics_pause(NativeCall& call) {
  Value n = call.args[0];
  if (!n.isUndefined() && !IsIntegralNumber(n)) return ThrowTypeError("Atomics.pause: argument must be an integral Number");
  return Value::undefined();
}

void installAtomics(Realm& realm) {
  Object* atomics = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  realm.setIntrinsic(Intrinsic::Atomics, atomics);
  defineGlobal(realm, PropertyKey(u"Atomics"), atomics);
  defineFunction(realm, atomics, PropertyKey(u"add"), 3, readModifyWrite<Op::Add>);
  defineFunction(realm, atomics, PropertyKey(u"and"), 3, readModifyWrite<Op::And>);
  defineFunction(realm, atomics, PropertyKey(u"compareExchange"), 4, Atomics_compareExchange);
  defineFunction(realm, atomics, PropertyKey(u"exchange"), 3, readModifyWrite<Op::Exchange>);
  defineFunction(realm, atomics, PropertyKey(u"isLockFree"), 1, Atomics_isLockFree);
  defineFunction(realm, atomics, PropertyKey(u"load"), 2, Atomics_load);
  defineFunction(realm, atomics, PropertyKey(u"notify"), 3, Atomics_notify);
  defineFunction(realm, atomics, PropertyKey(u"or"), 3, readModifyWrite<Op::Or>);
  defineFunction(realm, atomics, PropertyKey(u"pause"), 0, Atomics_pause);
  defineFunction(realm, atomics, PropertyKey(u"store"), 3, Atomics_store);
  defineFunction(realm, atomics, PropertyKey(u"sub"), 3, readModifyWrite<Op::Sub>);
  defineFunction(realm, atomics, PropertyKey(u"wait"), 4, Atomics_wait);
  defineFunction(realm, atomics, PropertyKey(u"xor"), 3, readModifyWrite<Op::Xor>);
  defineValue(atomics, PropertyKey(symbols().toStringTag), str(u"Atomics"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(Atomics, kOrderDefault, installAtomics);

}  // namespace snowman::builtins
