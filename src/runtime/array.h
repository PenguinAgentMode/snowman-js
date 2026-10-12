// Array exotic objects (10.4.2). Elements live in the PropertyMap (dense when they are plain data
// properties); "length" is a real own property, kept in step by [[DefineOwnProperty]].
#pragma once

#include <cstdint>

#include "runtime/object.h"

namespace snowman {

class ArrayObject : public Object {
 public:
  // Defines "length" (writable, non-enumerable, non-configurable) = length.
  ArrayObject(Object* prototype, uint32_t length = 0);

  Result<bool> DefineOwnProperty(const PropertyKey& key, const PropertyDescriptor& desc) override;

  // The current "length" value and whether it is writable (unobservable reads).
  uint32_t length() const;
  bool lengthWritable() const;
  // Sets "length" directly (no element deletion); only for building arrays.
  void setLengthDirect(uint32_t length);
};

// 10.4.2.4 ArraySetLength
Result<bool> ArraySetLength(ArrayObject* array, const PropertyDescriptor& desc);

}  // namespace snowman
