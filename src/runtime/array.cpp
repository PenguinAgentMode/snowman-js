// Array exotic objects (10.4.2).
#include "runtime/array.h"

#include <cmath>

#include "runtime/operations.h"
#include "runtime/property_map.h"
#include "runtime/symbol.h"
#include "runtime/vm.h"

namespace snowman {

namespace {
const PropertyKey& lengthKey() {
  static const PropertyKey key(u"length");
  return key;
}
}  // namespace

ArrayObject::ArrayObject(Object* prototype, uint32_t length) : Object(prototype) {
  properties_->put(lengthKey(), PropertySlot::data(Value::number(length), Attr::Writable));
}

uint32_t ArrayObject::length() const { return uint32_t(properties_->get(lengthKey())->value.asNumber()); }

bool ArrayObject::lengthWritable() const { return properties_->get(lengthKey())->writable(); }

void ArrayObject::setLengthDirect(uint32_t length) { properties_->setValue(lengthKey(), Value::number(length)); }

// 10.4.2.1 [[DefineOwnProperty]]
Result<bool> ArrayObject::DefineOwnProperty(const PropertyKey& key, const PropertyDescriptor& desc) {
  if (key == lengthKey()) return ArraySetLength(this, desc);
  if (key.isIndex()) {
    PropertySlot lengthSlot = *properties_->get(lengthKey());
    double length = lengthSlot.value.asNumber();
    uint32_t index = key.asIndex();
    if (index >= length && !lengthSlot.writable()) return false;
    bool succeeded = TRY(Object::DefineOwnProperty(key, desc));
    if (!succeeded) return false;
    if (index >= length) properties_->setValue(lengthKey(), Value::number(double(index) + 1));
    return true;
  }
  return Object::DefineOwnProperty(key, desc);
}

// 10.4.2.4 ArraySetLength
Result<bool> ArraySetLength(ArrayObject* array, const PropertyDescriptor& desc) {
  if (!desc.value) return array->Object::DefineOwnProperty(lengthKey(), desc);
  PropertyDescriptor newLenDesc = desc;
  uint32_t newLen = TRY(ToUint32(*desc.value));
  double numberLen = TRY(ToNumber(*desc.value));
  if (double(newLen) != numberLen) return ThrowRangeError("Invalid array length");
  newLenDesc.value = Value::number(newLen);
  PropertySlot oldLenDesc = *array->properties().get(lengthKey());
  double oldLen = oldLenDesc.value.asNumber();
  if (newLen >= oldLen) return array->Object::DefineOwnProperty(lengthKey(), newLenDesc);
  if (!oldLenDesc.writable()) return false;
  bool newWritable;
  if (!newLenDesc.writable || *newLenDesc.writable) {
    newWritable = true;
  } else {
    newWritable = false;
    newLenDesc.writable = true;
  }
  bool succeeded = TRY(array->Object::DefineOwnProperty(lengthKey(), newLenDesc));
  if (!succeeded) return false;
  std::optional<uint32_t> blocker = array->properties().truncateIndices(newLen);
  if (blocker) {
    newLenDesc.value = Value::number(double(*blocker) + 1);
    if (!newWritable) newLenDesc.writable = false;
    (void)TRY(array->Object::DefineOwnProperty(lengthKey(), newLenDesc));
    return false;
  }
  if (!newWritable) {
    PropertyDescriptor readOnly;
    readOnly.writable = false;
    (void)TRY(array->Object::DefineOwnProperty(lengthKey(), readOnly));
  }
  return true;
}

// 10.4.2.2 ArrayCreate
Result<Object*> ArrayCreate(double length, Object* prototype) {
  if (length > 4294967295.0) return ThrowRangeError("Invalid array length");
  if (!prototype) prototype = vm().currentRealm()->intrinsic(Intrinsic::ArrayPrototype);
  return make<ArrayObject>(prototype, uint32_t(length));
}

// 10.4.2.3 ArraySpeciesCreate
Result<Object*> ArraySpeciesCreate(Object* originalArray, double length) {
  bool isArray = TRY(IsArray(originalArray));
  if (!isArray) return ArrayCreate(length);
  Value c = TRY(Get(originalArray, PropertyKey(u"constructor")));
  if (IsConstructor(c)) {
    Realm* thisRealm = vm().currentRealm();
    Realm* realmC = TRY(GetFunctionRealm(c.asObject()));
    if (thisRealm != realmC && c.asObject() == realmC->intrinsic(Intrinsic::Array)) c = Value::undefined();
  }
  if (c.isObject()) {
    c = TRY(Get(c.asObject(), PropertyKey(vm().symbols.species)));
    if (c.isNull()) c = Value::undefined();
  }
  if (c.isUndefined()) return ArrayCreate(length);
  if (!IsConstructor(c)) return ThrowTypeError("Array species is not a constructor");
  return Construct(c.asObject(), {Value::number(length)});
}

}  // namespace snowman
