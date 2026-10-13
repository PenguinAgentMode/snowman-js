// ArrayBuffer / SharedArrayBuffer storage and raw element access (25.1.3).
#include "runtime/arraybuffer.h"

#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "runtime/bigint.h"
#include "runtime/operations.h"

namespace snowman {

namespace {
// The largest Data Block CreateByteDataBlock agrees to allocate.
constexpr double kMaxByteLength = double(1u << 30);
}  // namespace

ArrayBufferObject::ArrayBufferObject(Object* prototype, bool shared, size_t byteLength, std::optional<size_t> maxByteLength)
    : Object(prototype), shared_(shared), maxByteLength_(maxByteLength), data_(shared ? 0 : byteLength, 0) {
  if (shared) sharedBlock_ = std::make_shared<SharedBlock>(byteLength, maxByteLength.value_or(byteLength));
}

ArrayBufferObject::ArrayBufferObject(Object* prototype, std::shared_ptr<SharedBlock> block, std::optional<size_t> maxByteLength)
    : Object(prototype), shared_(true), maxByteLength_(maxByteLength), sharedBlock_(std::move(block)) {}

SharedBlock::SharedBlock(size_t byteLength, size_t maxByteLength) : maxByteLength_(maxByteLength), byteLength_(byteLength) {
  // Anonymous pages read as zero; MAP_NORESERVE leaves the untouched part of a growable buffer's reservation free.
  mapping_ = maxByteLength ? maxByteLength : 1;
  void* p = mmap(nullptr, mapping_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p != MAP_FAILED) data_ = static_cast<uint8_t*>(p);
}

SharedBlock::~SharedBlock() {
  if (data_) munmap(data_, mapping_);
}

bool SharedBlock::grow(size_t newByteLength) {
  size_t current = byteLength_.load();
  while (current <= newByteLength) {
    if (byteLength_.compare_exchange_weak(current, newByteLength)) return true;
  }
  return false;
}

void ArrayBufferObject::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(detachKey);
}

Result<ArrayBufferObject*> AllocateArrayBuffer(Object* constructor, double byteLength, std::optional<double> maxByteLength,
                                               bool shared) {
  if (maxByteLength && byteLength > *maxByteLength) return ThrowRangeError("byteLength exceeds maxByteLength");
  Intrinsic proto = shared ? Intrinsic::SharedArrayBufferPrototype : Intrinsic::ArrayBufferPrototype;
  Object* prototype = TRY(GetPrototypeFromConstructor(constructor, proto));
  if (byteLength > kMaxByteLength) return ThrowRangeError("Array buffer allocation failed");
  if (maxByteLength && *maxByteLength > kMaxByteLength) return ThrowRangeError("Array buffer allocation failed");
  std::optional<size_t> max;
  if (maxByteLength) max = size_t(*maxByteLength);
  ArrayBufferObject* buffer = make<ArrayBufferObject>(prototype, shared, size_t(byteLength), max);
  if (shared && !buffer->sharedBlock()->mapped()) return ThrowRangeError("Array buffer allocation failed");
  return buffer;
}

Result<void> DetachArrayBuffer(ArrayBufferObject* arrayBuffer, Value key) {
  if (arrayBuffer->isShared()) return ThrowTypeError("Cannot detach a SharedArrayBuffer");
  if (arrayBuffer->isImmutable()) return ThrowTypeError("Cannot detach an immutable ArrayBuffer");
  if (!SameValue(arrayBuffer->detachKey, key)) return ThrowTypeError("ArrayBuffer detach key mismatch");
  arrayBuffer->detach();
  return {};
}

bool IsDetachedBuffer(const ArrayBufferObject* buffer) { return buffer->isDetached(); }

Result<ArrayBufferObject*> AllocateImmutableArrayBuffer(Object* constructor, double byteLength, const uint8_t* fromBlock,
                                                        size_t fromIndex, size_t count) {
  ArrayBufferObject* newBuffer = TRY(AllocateArrayBuffer(constructor, byteLength));
  if (count > 0) std::memcpy(newBuffer->data(), fromBlock + fromIndex, count);
  newBuffer->setImmutable();
  return newBuffer;
}

bool IsImmutableBuffer(const ArrayBufferObject* buffer) { return buffer->isImmutable(); }

size_t ElementSize(ElementType type) {
  switch (type) {
    case ElementType::Int8:
    case ElementType::Uint8:
    case ElementType::Uint8C:
      return 1;
    case ElementType::Int16:
    case ElementType::Uint16:
    case ElementType::Float16:
      return 2;
    case ElementType::Int32:
    case ElementType::Uint32:
    case ElementType::Float32:
      return 4;
    case ElementType::Float64:
    case ElementType::BigInt64:
    case ElementType::BigUint64:
      return 8;
  }
  return 1;
}

bool IsBigIntElementType(ElementType type) { return type == ElementType::BigInt64 || type == ElementType::BigUint64; }

uint16_t DoubleToFloat16Bits(double value) {
  uint16_t sign = std::signbit(value) ? 0x8000 : 0;
  if (std::isnan(value)) return 0x7E00;
  double a = std::fabs(value);
  if (std::isinf(a) || a >= 65520.0) return sign | 0x7C00;
  if (a < std::ldexp(1.0, -14)) return sign | uint16_t(std::nearbyint(std::ldexp(a, 24)));
  int e;
  std::frexp(a, &e);
  int exponent = e - 1;
  double q = std::nearbyint(std::ldexp(a, 10 - exponent));
  if (q == 2048) {
    q = 1024;
    exponent++;
  }
  if (exponent > 15) return sign | 0x7C00;
  return sign | uint16_t((exponent + 15) << 10) | uint16_t(q - 1024);
}

double Float16BitsToDouble(uint16_t bits) {
  int exponent = (bits >> 10) & 0x1F;
  int mantissa = bits & 0x3FF;
  double value;
  if (exponent == 0) value = std::ldexp(double(mantissa), -24);
  else if (exponent == 31) value = mantissa ? NAN : INFINITY;
  else value = std::ldexp(double(1024 + mantissa), exponent - 25);
  return (bits & 0x8000) ? -value : value;
}

namespace {

// An element of a shared block is read and written with relaxed atomics (whole, when it is naturally aligned, as
// TypedArray elements are; else byte by byte), so a concurrent agent's access is not a data race.
template <class T>
void copyAtomic(void* to, const void* from, bool load) {
  T v;
  if (load) {
    v = std::atomic_ref<T>(*const_cast<T*>(static_cast<const T*>(from))).load(std::memory_order_relaxed);
    std::memcpy(to, &v, sizeof(T));
  } else {
    std::memcpy(&v, from, sizeof(T));
    std::atomic_ref<T>(*static_cast<T*>(to)).store(v, std::memory_order_relaxed);
  }
}

void copyShared(uint8_t* to, const uint8_t* from, size_t size, bool load) {
  const uint8_t* block = load ? from : to;
  if (reinterpret_cast<uintptr_t>(block) % size == 0) {
    switch (size) {
      case 1: return copyAtomic<uint8_t>(to, from, load);
      case 2: return copyAtomic<uint16_t>(to, from, load);
      case 4: return copyAtomic<uint32_t>(to, from, load);
      case 8: return copyAtomic<uint64_t>(to, from, load);
    }
  }
  for (size_t i = 0; i < size; i++) copyAtomic<uint8_t>(to + i, from + i, load);
}

void copyFromBlock(uint8_t* to, ArrayBufferObject* buffer, size_t byteIndex, size_t size) {
  if (buffer->isShared()) return copyShared(to, buffer->data() + byteIndex, size, true);
  std::memcpy(to, buffer->data() + byteIndex, size);
}

void copyToBlock(ArrayBufferObject* buffer, size_t byteIndex, const uint8_t* from, size_t size) {
  if (buffer->isShared()) return copyShared(buffer->data() + byteIndex, from, size, false);
  std::memcpy(buffer->data() + byteIndex, from, size);
}

}  // namespace

Value GetValueFromBuffer(ArrayBufferObject* buffer, size_t byteIndex, ElementType type, bool isLittleEndian) {
  uint8_t raw[8];
  size_t size = ElementSize(type);
  copyFromBlock(raw, buffer, byteIndex, size);
  if (!isLittleEndian) std::reverse(raw, raw + size);
  switch (type) {
    case ElementType::Int8: return Value::number(int8_t(raw[0]));
    case ElementType::Uint8:
    case ElementType::Uint8C: return Value::number(raw[0]);
    case ElementType::Int16: { int16_t v; std::memcpy(&v, raw, 2); return Value::number(v); }
    case ElementType::Uint16: { uint16_t v; std::memcpy(&v, raw, 2); return Value::number(v); }
    case ElementType::Int32: { int32_t v; std::memcpy(&v, raw, 4); return Value::number(v); }
    case ElementType::Uint32: { uint32_t v; std::memcpy(&v, raw, 4); return Value::number(v); }
    case ElementType::Float16: { uint16_t v; std::memcpy(&v, raw, 2); return Value::number(Float16BitsToDouble(v)); }
    case ElementType::Float32: { float v; std::memcpy(&v, raw, 4); return Value::number(double(v)); }
    case ElementType::Float64: { double v; std::memcpy(&v, raw, 8); return Value::number(v); }
    case ElementType::BigInt64: { int64_t v; std::memcpy(&v, raw, 8); return Value(BigInt::fromInt64(v)); }
    case ElementType::BigUint64: { uint64_t v; std::memcpy(&v, raw, 8); return Value(BigInt::fromUint64(v)); }
  }
  return Value::undefined();
}

void SetValueInBuffer(ArrayBufferObject* buffer, size_t byteIndex, ElementType type, Value value, bool isLittleEndian) {
  uint8_t raw[8];
  size_t size = ElementSize(type);
  double d = value.isNumber() ? value.asNumber() : 0;
  switch (type) {
    case ElementType::Int8:
    case ElementType::Uint8: raw[0] = uint8_t(DoubleToUint32(d)); break;
    case ElementType::Uint8C: {
      uint8_t c;
      if (std::isnan(d) || d <= 0) c = 0;
      else if (d >= 255) c = 255;
      else c = uint8_t(std::nearbyint(d));
      raw[0] = c;
      break;
    }
    case ElementType::Int16:
    case ElementType::Uint16: { uint16_t v = uint16_t(DoubleToUint32(d)); std::memcpy(raw, &v, 2); break; }
    case ElementType::Int32:
    case ElementType::Uint32: { uint32_t v = DoubleToUint32(d); std::memcpy(raw, &v, 4); break; }
    case ElementType::Float16: { uint16_t v = DoubleToFloat16Bits(d); std::memcpy(raw, &v, 2); break; }
    case ElementType::Float32: { float v = float(d); std::memcpy(raw, &v, 4); break; }
    case ElementType::Float64: std::memcpy(raw, &d, 8); break;
    case ElementType::BigInt64:
    case ElementType::BigUint64: { uint64_t v = value.asBigInt()->toUint64(); std::memcpy(raw, &v, 8); break; }
  }
  if (!isLittleEndian) std::reverse(raw, raw + size);
  copyToBlock(buffer, byteIndex, raw, size);
}

Result<Value> ToElementValue(ElementType type, Value value) {
  if (IsBigIntElementType(type)) return Value(TRY(ToBigInt(value)));
  return Value::number(TRY(ToNumber(value)));
}

}  // namespace snowman
