// SharedArrayBuffer (25.2), growable included. One agent only, so its memory is ordinary memory.
#include <algorithm>
#include <cstring>

#include "builtins/util.h"
#include "runtime/arraybuffer.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

// 25.2.3.1 SharedArrayBuffer ( length [ , options ] )
Result<Value> SharedArrayBuffer_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor SharedArrayBuffer requires 'new'");
  double byteLength = TRY(ToIndex(call.args[0]));
  std::optional<double> requestedMaxByteLength;
  if (call.args[1].isObject()) {
    Value maxByteLength = TRY(Get(call.args[1].asObject(), PropertyKey(u"maxByteLength")));
    if (!maxByteLength.isUndefined()) requestedMaxByteLength = TRY(ToIndex(maxByteLength));
  }
  return Value(TRY(AllocateArrayBuffer(call.newTarget, byteLength, requestedMaxByteLength, true)));
}

Result<Value> SharedArrayBuffer_species(NativeCall& call) { return call.thisValue; }

Result<ArrayBufferObject*> thisShared(Value v, const char* method) {
  ArrayBufferObject* b = v.isObject() ? v.asObject()->as<ArrayBufferObject>() : nullptr;
  if (!b || !b->isShared())
    return ThrowTypeError(std::string("SharedArrayBuffer.prototype.") + method + " called on incompatible receiver");
  return b;
}

Result<Value> SharedArrayBufferPrototype_byteLength(NativeCall& call) {
  ArrayBufferObject* b = TRY(thisShared(call.thisValue, "byteLength"));
  return num(double(b->byteLength()));
}

Result<Value> SharedArrayBufferPrototype_growable(NativeCall& call) {
  ArrayBufferObject* b = TRY(thisShared(call.thisValue, "growable"));
  return boolean(b->isResizable());
}

Result<Value> SharedArrayBufferPrototype_maxByteLength(NativeCall& call) {
  ArrayBufferObject* b = TRY(thisShared(call.thisValue, "maxByteLength"));
  return num(double(b->isResizable() ? b->maxByteLength() : b->byteLength()));
}

// 25.2.5.3 SharedArrayBuffer.prototype.grow ( newLength )
Result<Value> SharedArrayBufferPrototype_grow(NativeCall& call) {
  Value v = call.thisValue;
  ArrayBufferObject* b = v.isObject() ? v.asObject()->as<ArrayBufferObject>() : nullptr;
  if (!b || !b->isResizable()) return ThrowTypeError("SharedArrayBuffer.prototype.grow called on a non-growable buffer");
  if (!b->isShared()) return ThrowTypeError("SharedArrayBuffer.prototype.grow called on an ArrayBuffer");
  double newByteLength = TRY(ToIndex(call.args[0]));
  if (newByteLength > double(b->maxByteLength())) return ThrowRangeError("SharedArrayBuffer.prototype.grow: exceeds maxByteLength");
  if (newByteLength < double(b->byteLength())) return ThrowRangeError("SharedArrayBuffer.prototype.grow: cannot shrink");
  b->resize(size_t(newByteLength));
  return Value::undefined();
}

// 25.2.5.6 SharedArrayBuffer.prototype.slice ( start, end )
Result<Value> SharedArrayBufferPrototype_slice(NativeCall& call) {
  ArrayBufferObject* o = TRY(thisShared(call.thisValue, "slice"));
  double len = double(o->byteLength());
  double first = relativeIndex(TRY(ToIntegerOrInfinity(call.args[0])), len);
  double relativeEnd = call.args[1].isUndefined() ? len : TRY(ToIntegerOrInfinity(call.args[1]));
  double final = relativeIndex(relativeEnd, len);
  double newLen = std::max(final - first, 0.0);
  Object* ctor = TRY(SpeciesConstructor(o, intrinsic(Intrinsic::SharedArrayBuffer)));
  Object* created = TRY(Construct(ctor, {num(newLen)}));
  ArrayBufferObject* target = created->as<ArrayBufferObject>();
  if (!target || !target->isShared()) return ThrowTypeError("SharedArrayBuffer species did not create a SharedArrayBuffer");
  if (target == o) return ThrowTypeError("SharedArrayBuffer species returned the same buffer");
  if (double(target->byteLength()) < newLen) return ThrowTypeError("SharedArrayBuffer species created a too small buffer");
  if (newLen > 0) std::memmove(target->data(), o->data() + size_t(first), size_t(newLen));
  return Value(target);
}

void installSharedArrayBuffer(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor = defineConstructor(realm, Intrinsic::SharedArrayBuffer, SharedArrayBuffer_construct, 1,
                                            PropertyKey(u"SharedArrayBuffer"), proto, Intrinsic::SharedArrayBufferPrototype);
  defineAccessor(realm, ctor, PropertyKey(symbols().species), SharedArrayBuffer_species, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"byteLength"), SharedArrayBufferPrototype_byteLength, nullptr);
  defineFunction(realm, proto, PropertyKey(u"grow"), 1, SharedArrayBufferPrototype_grow);
  defineAccessor(realm, proto, PropertyKey(u"growable"), SharedArrayBufferPrototype_growable, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"maxByteLength"), SharedArrayBufferPrototype_maxByteLength, nullptr);
  defineFunction(realm, proto, PropertyKey(u"slice"), 2, SharedArrayBufferPrototype_slice);
  defineValue(proto, PropertyKey(symbols().toStringTag), str(u"SharedArrayBuffer"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(SharedArrayBuffer, kOrderDefault, installSharedArrayBuffer);

}  // namespace snowman::builtins
