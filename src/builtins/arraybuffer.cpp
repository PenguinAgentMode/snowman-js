// ArrayBuffer (25.1), resizable included.
#include <algorithm>
#include <cstring>

#include "builtins/util.h"
#include "runtime/arraybuffer.h"
#include "runtime/function.h"
#include "runtime/typedarray.h"

namespace snowman::builtins {

namespace {

// 25.1.3.7 GetArrayBufferMaxByteLengthOption ( options )
Result<std::optional<double>> GetArrayBufferMaxByteLengthOption(Value options) {
  if (!options.isObject()) return std::optional<double>();
  Value maxByteLength = TRY(Get(options.asObject(), PropertyKey(u"maxByteLength")));
  if (maxByteLength.isUndefined()) return std::optional<double>();
  return std::optional<double>(TRY(ToIndex(maxByteLength)));
}

// 25.1.4.1 ArrayBuffer ( length [ , options ] )
Result<Value> ArrayBuffer_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor ArrayBuffer requires 'new'");
  double byteLength = TRY(ToIndex(call.args[0]));
  std::optional<double> requestedMaxByteLength = TRY(GetArrayBufferMaxByteLengthOption(call.args[1]));
  return Value(TRY(AllocateArrayBuffer(call.newTarget, byteLength, requestedMaxByteLength)));
}

// 25.1.5.1 ArrayBuffer.isView ( arg )
Result<Value> ArrayBuffer_isView(NativeCall& call) {
  Value arg = call.args[0];
  return boolean(arg.isObject() && (arg.asObject()->is<TypedArrayObject>() || arg.asObject()->is<DataViewObject>()));
}

Result<Value> ArrayBuffer_species(NativeCall& call) { return call.thisValue; }

// RequireInternalSlot(O, [[ArrayBufferData]]) and IsSharedArrayBuffer(O) is false.
Result<ArrayBufferObject*> thisArrayBuffer(Value v, const char* method) {
  ArrayBufferObject* b = v.isObject() ? v.asObject()->as<ArrayBufferObject>() : nullptr;
  if (!b || b->isShared()) return ThrowTypeError(std::string("ArrayBuffer.prototype.") + method + " called on incompatible receiver");
  return b;
}

Result<Value> ArrayBufferPrototype_byteLength(NativeCall& call) {
  ArrayBufferObject* b = TRY(thisArrayBuffer(call.thisValue, "byteLength"));
  return num(double(b->byteLength()));
}

Result<Value> ArrayBufferPrototype_maxByteLength(NativeCall& call) {
  ArrayBufferObject* b = TRY(thisArrayBuffer(call.thisValue, "maxByteLength"));
  if (b->isDetached()) return num(0);
  return num(double(b->isResizable() ? b->maxByteLength() : b->byteLength()));
}

Result<Value> ArrayBufferPrototype_resizable(NativeCall& call) {
  ArrayBufferObject* b = TRY(thisArrayBuffer(call.thisValue, "resizable"));
  return boolean(b->isResizable());
}

Result<Value> ArrayBufferPrototype_detached(NativeCall& call) {
  ArrayBufferObject* b = TRY(thisArrayBuffer(call.thisValue, "detached"));
  return boolean(b->isDetached());
}

// get ArrayBuffer.prototype.immutable (immutable-arraybuffer)
Result<Value> ArrayBufferPrototype_immutable(NativeCall& call) {
  ArrayBufferObject* b = TRY(thisArrayBuffer(call.thisValue, "immutable"));
  return boolean(IsImmutableBuffer(b));
}

// 25.1.6.6 ArrayBuffer.prototype.resize ( newLength )
Result<Value> ArrayBufferPrototype_resize(NativeCall& call) {
  ArrayBufferObject* b = TRY(thisArrayBuffer(call.thisValue, "resize"));
  if (!b->isResizable()) return ThrowTypeError("ArrayBuffer.prototype.resize: buffer is not resizable");
  double newByteLength = TRY(ToIndex(call.args[0]));
  if (b->isDetached()) return ThrowTypeError("ArrayBuffer.prototype.resize: buffer is detached");
  if (newByteLength > double(b->maxByteLength())) return ThrowRangeError("ArrayBuffer.prototype.resize: new length exceeds maxByteLength");
  b->resize(size_t(newByteLength));
  return Value::undefined();
}

struct Bounds {
  double from, to;
};

// ResolveBounds ( len, start, end )
Result<Bounds> ResolveBounds(double len, Value start, Value end) {
  double from = relativeIndex(TRY(ToIntegerOrInfinity(start)), len);
  double relativeEnd = end.isUndefined() ? len : TRY(ToIntegerOrInfinity(end));
  return Bounds{from, relativeIndex(relativeEnd, len)};
}

// 25.1.6.7 ArrayBuffer.prototype.slice ( start, end )
Result<Value> ArrayBufferPrototype_slice(NativeCall& call) {
  ArrayBufferObject* o = TRY(thisArrayBuffer(call.thisValue, "slice"));
  if (o->isDetached()) return ThrowTypeError("ArrayBuffer.prototype.slice: buffer is detached");
  double len = double(o->byteLength());
  auto [first, final] = TRY(ResolveBounds(len, call.args[0], call.args[1]));
  double newLen = std::max(final - first, 0.0);
  Object* ctor = TRY(SpeciesConstructor(o, intrinsic(Intrinsic::ArrayBuffer)));
  Object* created = TRY(Construct(ctor, {num(newLen)}));
  ArrayBufferObject* target = created->as<ArrayBufferObject>();
  if (!target || target->isShared()) return ThrowTypeError("ArrayBuffer species did not create an ArrayBuffer");
  if (target->isDetached()) return ThrowTypeError("ArrayBuffer species created a detached buffer");
  if (IsImmutableBuffer(target)) return ThrowTypeError("ArrayBuffer species created an immutable buffer");
  if (target == o) return ThrowTypeError("ArrayBuffer species returned the same buffer");
  if (double(target->byteLength()) < newLen) return ThrowTypeError("ArrayBuffer species created a too small buffer");
  if (o->isDetached()) return ThrowTypeError("ArrayBuffer.prototype.slice: buffer is detached");
  double currentLen = double(o->byteLength());
  if (first < currentLen) {
    size_t count = size_t(std::min(newLen, currentLen - first));
    // An empty buffer's data() may be null, and memmove/memcpy's pointers must not be, even for 0 bytes.
    if (count > 0) std::memmove(target->data(), o->data() + size_t(first), count);
  }
  return Value(target);
}

// ArrayBuffer.prototype.sliceToImmutable ( start, end ) (immutable-arraybuffer)
Result<Value> ArrayBufferPrototype_sliceToImmutable(NativeCall& call) {
  ArrayBufferObject* o = TRY(thisArrayBuffer(call.thisValue, "sliceToImmutable"));
  if (o->isDetached()) return ThrowTypeError("ArrayBuffer.prototype.sliceToImmutable: buffer is detached");
  double len = double(o->byteLength());
  auto [first, final] = TRY(ResolveBounds(len, call.args[0], call.args[1]));
  double newLen = std::max(final - first, 0.0);
  if (o->isDetached()) return ThrowTypeError("ArrayBuffer.prototype.sliceToImmutable: buffer is detached");
  double currentLen = double(o->byteLength());
  if (currentLen < final) return ThrowRangeError("ArrayBuffer.prototype.sliceToImmutable: buffer shrank below the end");
  return Value(TRY(AllocateImmutableArrayBuffer(intrinsic(Intrinsic::ArrayBuffer), newLen, o->data(), size_t(first),
                                                size_t(newLen))));
}

enum class CopyMode { PreserveResizability, FixedLength, Immutable };

// 25.1.3.3 ArrayBufferCopyAndDetach ( arrayBuffer, newLength, preserveResizability )
Result<Value> ArrayBufferCopyAndDetach(NativeCall& call, CopyMode mode, const char* method) {
  ArrayBufferObject* o = TRY(thisArrayBuffer(call.thisValue, method));
  double newByteLength = call.args[0].isUndefined() ? double(o->byteLength()) : TRY(ToIndex(call.args[0]));
  if (o->isDetached()) return ThrowTypeError(std::string("ArrayBuffer.prototype.") + method + ": buffer is detached");
  if (IsImmutableBuffer(o)) return ThrowTypeError(std::string("ArrayBuffer.prototype.") + method + ": buffer is immutable");
  if (!o->detachKey.isUndefined()) return ThrowTypeError("ArrayBuffer cannot be detached");
  size_t copyLength = size_t(std::min(newByteLength, double(o->byteLength())));
  ArrayBufferObject* newBuffer;
  if (mode == CopyMode::Immutable) {
    newBuffer = TRY(AllocateImmutableArrayBuffer(intrinsic(Intrinsic::ArrayBuffer), newByteLength, o->data(), 0, copyLength));
  } else {
    std::optional<double> newMaxByteLength;
    if (mode == CopyMode::PreserveResizability && o->isResizable()) newMaxByteLength = double(o->maxByteLength());
    newBuffer = TRY(AllocateArrayBuffer(intrinsic(Intrinsic::ArrayBuffer), newByteLength, newMaxByteLength));
    if (copyLength > 0) std::memcpy(newBuffer->data(), o->data(), copyLength);
  }
  TRY(DetachArrayBuffer(o));
  return Value(newBuffer);
}

Result<Value> ArrayBufferPrototype_transfer(NativeCall& call) {
  return ArrayBufferCopyAndDetach(call, CopyMode::PreserveResizability, "transfer");
}
Result<Value> ArrayBufferPrototype_transferToFixedLength(NativeCall& call) {
  return ArrayBufferCopyAndDetach(call, CopyMode::FixedLength, "transferToFixedLength");
}
Result<Value> ArrayBufferPrototype_transferToImmutable(NativeCall& call) {
  return ArrayBufferCopyAndDetach(call, CopyMode::Immutable, "transferToImmutable");
}

void installArrayBuffer(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor = defineConstructor(realm, Intrinsic::ArrayBuffer, ArrayBuffer_construct, 1,
                                            PropertyKey(u"ArrayBuffer"), proto, Intrinsic::ArrayBufferPrototype);
  defineFunction(realm, ctor, PropertyKey(u"isView"), 1, ArrayBuffer_isView);
  defineAccessor(realm, ctor, PropertyKey(symbols().species), ArrayBuffer_species, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"byteLength"), ArrayBufferPrototype_byteLength, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"detached"), ArrayBufferPrototype_detached, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"immutable"), ArrayBufferPrototype_immutable, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"maxByteLength"), ArrayBufferPrototype_maxByteLength, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"resizable"), ArrayBufferPrototype_resizable, nullptr);
  defineFunction(realm, proto, PropertyKey(u"resize"), 1, ArrayBufferPrototype_resize);
  defineFunction(realm, proto, PropertyKey(u"slice"), 2, ArrayBufferPrototype_slice);
  defineFunction(realm, proto, PropertyKey(u"sliceToImmutable"), 2, ArrayBufferPrototype_sliceToImmutable);
  defineFunction(realm, proto, PropertyKey(u"transfer"), 0, ArrayBufferPrototype_transfer);
  defineFunction(realm, proto, PropertyKey(u"transferToFixedLength"), 0, ArrayBufferPrototype_transferToFixedLength);
  defineFunction(realm, proto, PropertyKey(u"transferToImmutable"), 0, ArrayBufferPrototype_transferToImmutable);
  defineValue(proto, PropertyKey(symbols().toStringTag), str(u"ArrayBuffer"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(ArrayBuffer, kOrderDefault, installArrayBuffer);

}  // namespace snowman::builtins
