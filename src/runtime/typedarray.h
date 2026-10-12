// TypedArray exotic objects (10.4.5) and the abstract operations over them (23.2.4, 10.4.5.9–10.4.5.17).
#pragma once

#include <optional>

#include "runtime/arraybuffer.h"
#include "runtime/object.h"
#include "runtime/realm.h"

namespace snowman {

class TypedArrayObject : public Object {
 public:
  TypedArrayObject(Object* prototype, ElementType type) : Object(prototype), elementType(type) {}

  Result<std::optional<PropertyDescriptor>> GetOwnProperty(const PropertyKey& key) override;
  Result<bool> HasProperty(const PropertyKey& key) override;
  Result<bool> DefineOwnProperty(const PropertyKey& key, const PropertyDescriptor& desc) override;
  Result<Value> Get(const PropertyKey& key, Value receiver) override;
  Result<bool> Set(const PropertyKey& key, Value value, Value receiver) override;
  Result<bool> Delete(const PropertyKey& key) override;
  Result<std::vector<PropertyKey>> OwnPropertyKeys() override;
  void trace(Tracer& tracer) override;

  ElementType elementType;             // [[TypedArrayName]] / [[ContentType]]
  ArrayBufferObject* buffer = nullptr;  // [[ViewedArrayBuffer]]
  double byteOffset = 0;               // [[ByteOffset]]
  std::optional<double> arrayLength;   // [[ArrayLength]]; nullopt is ~auto~ (length-tracking)
  std::optional<double> byteLength;    // [[ByteLength]]; nullopt is ~auto~
};

// The TypedArray constructor intrinsic and its prototype for each element type.
Intrinsic TypedArrayConstructorIntrinsic(ElementType type);
Intrinsic TypedArrayPrototypeIntrinsic(ElementType type);
const char16_t* TypedArrayName(ElementType type);

// 10.4.5.10 IsTypedArrayOutOfBounds / 10.4.5.12 TypedArrayLength over the buffer's current length.
bool IsTypedArrayOutOfBounds(const TypedArrayObject* o);
double TypedArrayLength(const TypedArrayObject* o);  // 0 when out of bounds
double TypedArrayByteLength(const TypedArrayObject* o);
// 10.4.5.14 IsValidIntegerIndex
bool IsValidIntegerIndex(const TypedArrayObject* o, double index);
// 10.4.5.15 TypedArrayGetElement / 10.4.5.16 TypedArraySetElement
Value TypedArrayGetElement(TypedArrayObject* o, double index);
Result<void> TypedArraySetElement(TypedArrayObject* o, double index, Value value);
// 23.2.4.4 ValidateTypedArray: TypeError unless o is an in-bounds TypedArray.
Result<TypedArrayObject*> ValidateTypedArray(Value o);

}  // namespace snowman
