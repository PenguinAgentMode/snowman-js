// ArrayBuffer / SharedArrayBuffer objects (25.1, 25.2) and the raw element access TypedArrays, DataView and
// Atomics share. Shared memory is single-agent here, so SharedArrayBuffer is a buffer that cannot detach.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "runtime/object.h"

namespace snowman {

class ArrayBufferObject : public Object {
 public:
  ArrayBufferObject(Object* prototype, bool shared, size_t byteLength, std::optional<size_t> maxByteLength);

  bool isShared() const { return shared_; }
  bool isDetached() const { return detached_; }
  // Resizable (ArrayBuffer) or growable (SharedArrayBuffer): [[ArrayBufferMaxByteLength]] is present.
  bool isResizable() const { return maxByteLength_.has_value(); }
  size_t maxByteLength() const { return maxByteLength_.value_or(data_.size()); }
  size_t byteLength() const { return detached_ ? 0 : data_.size(); }
  uint8_t* data() { return data_.data(); }
  void resize(size_t newByteLength) { data_.resize(newByteLength, 0); }

  // [[ArrayBufferIsImmutable]] (immutable-arraybuffer): set once, by AllocateImmutableArrayBuffer. An immutable
  // buffer is fixed-length and can't be detached; nothing writes to its data after allocation.
  bool isImmutable() const { return immutable_; }
  void setImmutable() { immutable_ = true; }

  Value detachKey;  // [[ArrayBufferDetachKey]]
  void detach() {
    detached_ = true;
    data_.clear();
    data_.shrink_to_fit();
  }
  void trace(Tracer& tracer) override;

 private:
  bool shared_;
  bool detached_ = false;
  bool immutable_ = false;
  std::optional<size_t> maxByteLength_;
  std::vector<uint8_t> data_;
};

// A DataView (25.3): [[ViewedArrayBuffer]], [[ByteLength]] (nullopt = ~auto~), [[ByteOffset]].
class DataViewObject : public Object {
 public:
  explicit DataViewObject(Object* prototype) : Object(prototype) {}
  ArrayBufferObject* buffer = nullptr;
  std::optional<double> byteLength;
  double byteOffset = 0;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(reinterpret_cast<Cell*>(buffer));
  }
};

// 25.1.3.1 AllocateArrayBuffer, 25.2.2.1 AllocateSharedArrayBuffer (RangeError if too large).
Result<ArrayBufferObject*> AllocateArrayBuffer(Object* constructor, double byteLength,
                                               std::optional<double> maxByteLength = std::nullopt, bool shared = false);
// 25.1.3.5 DetachArrayBuffer ( arrayBuffer [ , key ] ): TypeError if the key does not match or it is shared.
Result<void> DetachArrayBuffer(ArrayBufferObject* arrayBuffer, Value key = Value::undefined());
bool IsDetachedBuffer(const ArrayBufferObject* buffer);
// AllocateImmutableArrayBuffer ( constructor, byteLength, fromBlock, fromIndex, count ) and IsImmutableBuffer
// (immutable-arraybuffer). count bytes from fromBlock + fromIndex are copied in; the rest are zero.
Result<ArrayBufferObject*> AllocateImmutableArrayBuffer(Object* constructor, double byteLength, const uint8_t* fromBlock,
                                                        size_t fromIndex, size_t count);
bool IsImmutableBuffer(const ArrayBufferObject* buffer);

// The element types of Table 71.
enum class ElementType : uint8_t {
  Int8, Uint8, Uint8C, Int16, Uint16, Int32, Uint32, Float16, Float32, Float64, BigInt64, BigUint64,
};
size_t ElementSize(ElementType type);
bool IsBigIntElementType(ElementType type);
// 25.1.3.17 GetValueFromBuffer / 25.1.3.18 SetValueInBuffer (little-endian unless isLittleEndian is false).
// value must already be a Number (or a BigInt for the BigInt types).
Value GetValueFromBuffer(ArrayBufferObject* buffer, size_t byteIndex, ElementType type, bool isLittleEndian = true);
void SetValueInBuffer(ArrayBufferObject* buffer, size_t byteIndex, ElementType type, Value value, bool isLittleEndian = true);
// 7.1.x the conversion SetValueInBuffer's caller performs: ToBigInt for BigInt types, ToNumber otherwise.
Result<Value> ToElementValue(ElementType type, Value value);

// IEEE 754 binary16 <-> double.
uint16_t DoubleToFloat16Bits(double value);
double Float16BitsToDouble(uint16_t bits);

}  // namespace snowman
