// Primitive wrapper objects; String exotic objects (10.4.3).
#include "runtime/wrappers.h"

#include "runtime/bigint.h"
#include "runtime/operations.h"
#include "runtime/property_map.h"
#include "runtime/string.h"
#include "runtime/symbol.h"

namespace snowman {

void SymbolObject::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(reinterpret_cast<Cell*>(symbolData));
}

void BigIntObject::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(reinterpret_cast<Cell*>(bigintData));
}

StringObject::StringObject(Object* prototype, String* value) : Object(prototype), stringData(value) {
  properties_->put(PropertyKey(u"length"), PropertySlot::data(Value::number(double(value->length())), Attr::None));
}

void StringObject::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(reinterpret_cast<Cell*>(stringData));
}

std::optional<PropertyDescriptor> StringObject::StringGetOwnProperty(const PropertyKey& key) const {
  if (!key.isIndex()) return std::nullopt;
  uint32_t index = key.asIndex();
  if (index >= stringData->length()) return std::nullopt;
  char16_t unit = stringData->at(index);
  return PropertyDescriptor::data(newString(std::u16string_view(&unit, 1)), Attr::Enumerable);
}

// 10.4.3.1 [[GetOwnProperty]]
Result<std::optional<PropertyDescriptor>> StringObject::GetOwnProperty(const PropertyKey& key) {
  auto desc = TRY(Object::GetOwnProperty(key));
  if (desc) return desc;
  return StringGetOwnProperty(key);
}

// 10.4.3.2 [[DefineOwnProperty]]
Result<bool> StringObject::DefineOwnProperty(const PropertyKey& key, const PropertyDescriptor& desc) {
  auto stringDesc = StringGetOwnProperty(key);
  if (stringDesc) return IsCompatiblePropertyDescriptor(extensible_, desc, stringDesc);
  return Object::DefineOwnProperty(key, desc);
}

// 10.4.3.3 [[OwnPropertyKeys]]
Result<std::vector<PropertyKey>> StringObject::OwnPropertyKeys() {
  std::vector<PropertyKey> keys;
  uint32_t len = uint32_t(stringData->length());
  keys.reserve(len + properties_->size());
  for (uint32_t i = 0; i < len; i++) keys.push_back(PropertyKey::index(i));
  for (const PropertyKey& key : properties_->keys()) {
    if (key.isIndex() && key.asIndex() < len) continue;
    keys.push_back(key);
  }
  return keys;
}

}  // namespace snowman
