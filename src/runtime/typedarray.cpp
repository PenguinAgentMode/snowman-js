// TypedArray exotic objects (10.4.5).
#include "runtime/typedarray.h"

#include <cmath>

#include "runtime/operations.h"
#include "runtime/property_map.h"
#include "runtime/string.h"

namespace snowman {

namespace {

// A numeric property key (10.4.5: CanonicalNumericIndexString is not undefined), as a Number.
std::optional<double> numericIndex(const PropertyKey& key) {
  if (key.isIndex()) return double(key.asIndex());
  if (key.isString()) return CanonicalNumericIndexString(key.asString());
  return std::nullopt;
}

}  // namespace

void TypedArrayObject::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(reinterpret_cast<Cell*>(buffer));
}

Intrinsic TypedArrayConstructorIntrinsic(ElementType type) {
  switch (type) {
    case ElementType::Int8: return Intrinsic::Int8Array;
    case ElementType::Uint8: return Intrinsic::Uint8Array;
    case ElementType::Uint8C: return Intrinsic::Uint8ClampedArray;
    case ElementType::Int16: return Intrinsic::Int16Array;
    case ElementType::Uint16: return Intrinsic::Uint16Array;
    case ElementType::Int32: return Intrinsic::Int32Array;
    case ElementType::Uint32: return Intrinsic::Uint32Array;
    case ElementType::Float16: return Intrinsic::Float16Array;
    case ElementType::Float32: return Intrinsic::Float32Array;
    case ElementType::Float64: return Intrinsic::Float64Array;
    case ElementType::BigInt64: return Intrinsic::BigInt64Array;
    case ElementType::BigUint64: return Intrinsic::BigUint64Array;
  }
  return Intrinsic::Uint8Array;
}

Intrinsic TypedArrayPrototypeIntrinsic(ElementType type) {
  return Intrinsic(uint16_t(TypedArrayConstructorIntrinsic(type)) + 1);
}

const char16_t* TypedArrayName(ElementType type) {
  switch (type) {
    case ElementType::Int8: return u"Int8Array";
    case ElementType::Uint8: return u"Uint8Array";
    case ElementType::Uint8C: return u"Uint8ClampedArray";
    case ElementType::Int16: return u"Int16Array";
    case ElementType::Uint16: return u"Uint16Array";
    case ElementType::Int32: return u"Int32Array";
    case ElementType::Uint32: return u"Uint32Array";
    case ElementType::Float16: return u"Float16Array";
    case ElementType::Float32: return u"Float32Array";
    case ElementType::Float64: return u"Float64Array";
    case ElementType::BigInt64: return u"BigInt64Array";
    case ElementType::BigUint64: return u"BigUint64Array";
  }
  return u"";
}

bool IsTypedArrayOutOfBounds(const TypedArrayObject* o) {
  if (o->buffer->isDetached()) return true;
  double bufferByteLength = double(o->buffer->byteLength());
  double byteOffsetStart = o->byteOffset;
  double byteOffsetEnd = o->arrayLength ? byteOffsetStart + *o->arrayLength * double(ElementSize(o->elementType))
                                        : bufferByteLength;
  return byteOffsetStart > bufferByteLength || byteOffsetEnd > bufferByteLength;
}

double TypedArrayLength(const TypedArrayObject* o) {
  if (IsTypedArrayOutOfBounds(o)) return 0;
  if (o->arrayLength) return *o->arrayLength;
  double elementSize = double(ElementSize(o->elementType));
  return std::floor((double(o->buffer->byteLength()) - o->byteOffset) / elementSize);
}

double TypedArrayByteLength(const TypedArrayObject* o) {
  if (IsTypedArrayOutOfBounds(o)) return 0;
  return TypedArrayLength(o) * double(ElementSize(o->elementType));
}

bool IsValidIntegerIndex(const TypedArrayObject* o, double index) {
  if (o->buffer->isDetached()) return false;
  if (!std::isfinite(index) || std::trunc(index) != index) return false;
  if (index == 0 && std::signbit(index)) return false;
  if (IsTypedArrayOutOfBounds(o)) return false;
  return index >= 0 && index < TypedArrayLength(o);
}

Value TypedArrayGetElement(TypedArrayObject* o, double index) {
  if (!IsValidIntegerIndex(o, index)) return Value::undefined();
  size_t byteIndex = size_t(index) * ElementSize(o->elementType) + size_t(o->byteOffset);
  return GetValueFromBuffer(o->buffer, byteIndex, o->elementType);
}

Result<void> TypedArraySetElement(TypedArrayObject* o, double index, Value value) {
  Value numValue = TRY(ToElementValue(o->elementType, value));
  if (IsValidIntegerIndex(o, index)) {
    size_t byteIndex = size_t(index) * ElementSize(o->elementType) + size_t(o->byteOffset);
    SetValueInBuffer(o->buffer, byteIndex, o->elementType, numValue);
  }
  return {};
}

Result<TypedArrayObject*> ValidateTypedArray(Value o) {
  TypedArrayObject* ta = o.isObject() ? o.asObject()->as<TypedArrayObject>() : nullptr;
  if (!ta) return ThrowTypeError("this is not a typed array.");
  if (IsTypedArrayOutOfBounds(ta)) return ThrowTypeError("typed array is detached or out of bounds");
  return ta;
}

// 10.4.5.1 [[GetOwnProperty]]
Result<std::optional<PropertyDescriptor>> TypedArrayObject::GetOwnProperty(const PropertyKey& key) {
  if (auto index = numericIndex(key)) {
    Value value = TypedArrayGetElement(this, *index);
    if (value.isUndefined()) return std::optional<PropertyDescriptor>();
    return std::optional<PropertyDescriptor>(PropertyDescriptor::data(value, Attr::All));
  }
  return Object::GetOwnProperty(key);
}

// 10.4.5.2 [[HasProperty]]
Result<bool> TypedArrayObject::HasProperty(const PropertyKey& key) {
  if (auto index = numericIndex(key)) return IsValidIntegerIndex(this, *index);
  return Object::HasProperty(key);
}

// 10.4.5.3 [[DefineOwnProperty]]
Result<bool> TypedArrayObject::DefineOwnProperty(const PropertyKey& key, const PropertyDescriptor& desc) {
  if (auto index = numericIndex(key)) {
    if (!IsValidIntegerIndex(this, *index)) return false;
    if (desc.configurable && !*desc.configurable) return false;
    if (desc.enumerable && !*desc.enumerable) return false;
    if (desc.isAccessorDescriptor()) return false;
    if (desc.writable && !*desc.writable) return false;
    if (desc.value) TRY(TypedArraySetElement(this, *index, *desc.value));
    return true;
  }
  return Object::DefineOwnProperty(key, desc);
}

// 10.4.5.4 [[Get]]
Result<Value> TypedArrayObject::Get(const PropertyKey& key, Value receiver) {
  if (auto index = numericIndex(key)) return TypedArrayGetElement(this, *index);
  return Object::Get(key, receiver);
}

// 10.4.5.5 [[Set]]
Result<bool> TypedArrayObject::Set(const PropertyKey& key, Value value, Value receiver) {
  if (auto index = numericIndex(key)) {
    if (receiver.isObject() && receiver.asObject() == this) {
      TRY(TypedArraySetElement(this, *index, value));
      return true;
    }
    if (!IsValidIntegerIndex(this, *index)) return true;
  }
  return Object::Set(key, value, receiver);
}

// 10.4.5.6 [[Delete]]
Result<bool> TypedArrayObject::Delete(const PropertyKey& key) {
  if (auto index = numericIndex(key)) return !IsValidIntegerIndex(this, *index);
  return Object::Delete(key);
}

// 10.4.5.2 [[PreventExtensions]]: a length-tracking or resizable-backed TypedArray cannot be made
// non-extensible (IsTypedArrayFixedLength).
Result<bool> TypedArrayObject::PreventExtensions() {
  if (!arrayLength) return false;
  if (buffer->isResizable() && !buffer->isShared()) return false;
  return Object::PreventExtensions();
}

// 10.4.5.7 [[OwnPropertyKeys]]
Result<std::vector<PropertyKey>> TypedArrayObject::OwnPropertyKeys() {
  std::vector<PropertyKey> keys;
  if (!IsTypedArrayOutOfBounds(this)) {
    double length = TypedArrayLength(this);
    for (double i = 0; i < length; i++) keys.push_back(PropertyKey::index(uint32_t(i)));
  }
  for (const PropertyKey& key : properties_->keys()) keys.push_back(key);
  return keys;
}

}  // namespace snowman
