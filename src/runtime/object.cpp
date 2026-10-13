// Ordinary objects: the essential internal methods of ECMA-262 10.1, and the descriptor algorithms.
#include "runtime/object.h"

#include <typeinfo>

#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/property_map.h"
#include "runtime/vm.h"

namespace snowman {

Object::Object(Object* prototype) : prototype_(prototype), properties_(std::make_unique<PropertyMap>()) {}

Object::~Object() = default;

void Object::trace(Tracer& tracer) {
  tracer.visit(reinterpret_cast<Cell*>(prototype_));
  tracer.visit(privateElements);
  properties_->trace(tracer);
}

// 10.1.1 OrdinaryGetPrototypeOf
Result<Object*> Object::GetPrototypeOf() { return prototype_; }

// 10.1.2.1 OrdinarySetPrototypeOf
Result<bool> Object::SetPrototypeOf(Object* v) {
  Object* current = prototype_;
  if (v == current) return true;
  if (!extensible_) return false;
  Object* p = v;
  while (p) {
    if (p == this) return false;
    if (!p->hasOrdinaryGetPrototypeOf()) break;
    p = p->prototype_;
  }
  prototype_ = v;
  return true;
}

// 10.1.3.1 OrdinaryIsExtensible
Result<bool> Object::IsExtensible() { return extensible_; }

// 10.1.4.1 OrdinaryPreventExtensions
Result<bool> Object::PreventExtensions() {
  extensible_ = false;
  return true;
}

// 10.1.5.1 OrdinaryGetOwnProperty
Result<std::optional<PropertyDescriptor>> Object::GetOwnProperty(const PropertyKey& key) {
  auto slot = properties_->get(key);
  if (!slot) return std::optional<PropertyDescriptor>();
  return std::optional<PropertyDescriptor>(slot->toDescriptor());
}

// 10.1.6.1 OrdinaryDefineOwnProperty
Result<bool> Object::DefineOwnProperty(const PropertyKey& key, const PropertyDescriptor& desc) {
  auto current = TRY(GetOwnProperty(key));
  bool extensible = TRY(IsExtensible());
  return ValidateAndApplyPropertyDescriptor(this, key, extensible, desc, current);
}

// 10.1.7.1 OrdinaryHasProperty
Result<bool> Object::HasProperty(const PropertyKey& key) {
  auto hasOwn = TRY(GetOwnProperty(key));
  if (hasOwn) return true;
  Object* parent = TRY(GetPrototypeOf());
  if (parent) return parent->HasProperty(key);
  return false;
}

// 10.1.8.1 OrdinaryGet
Result<Value> Object::Get(const PropertyKey& key, Value receiver) {
  auto desc = TRY(GetOwnProperty(key));
  if (!desc) {
    Object* parent = TRY(GetPrototypeOf());
    if (!parent) return Value::undefined();
    return parent->Get(key, receiver);
  }
  if (desc->isDataDescriptor()) return *desc->value;
  Value getter = desc->get.value_or(Value::undefined());
  if (getter.isUndefined()) return Value::undefined();
  return snowman::Call(getter, receiver);
}

bool Object::hasOrdinaryPropertyStorage() const { return typeid(*this) == typeid(Object); }

// 10.1.9.1 OrdinarySet
Result<bool> Object::Set(const PropertyKey& key, Value value, Value receiver) {
  // An own writable data property of an ordinary receiver: OrdinarySet ends in OrdinaryDefineOwnProperty
  // replacing just the value.
  if (receiver.isObject() && receiver.asObject() == this && hasOrdinaryPropertyStorage() &&
      properties_->setWritableValue(key, value))
    return true;
  auto ownDesc = TRY(GetOwnProperty(key));
  return OrdinarySetWithOwnDescriptor(this, key, value, receiver, ownDesc);
}

// 10.1.9.2 OrdinarySetWithOwnDescriptor
Result<bool> OrdinarySetWithOwnDescriptor(Object* object, const PropertyKey& key, Value value, Value receiver,
                                          std::optional<PropertyDescriptor> ownDesc) {
  if (!ownDesc) {
    Object* parent = TRY(object->GetPrototypeOf());
    if (parent) return parent->Set(key, value, receiver);
    ownDesc = PropertyDescriptor::data(Value::undefined(), Attr::All);
  }
  if (ownDesc->isDataDescriptor()) {
    if (!ownDesc->writable.value_or(false)) return false;
    if (!receiver.isObject()) return false;
    Object* r = receiver.asObject();
    auto existing = TRY(r->GetOwnProperty(key));
    if (existing) {
      if (existing->isAccessorDescriptor()) return false;
      if (!existing->writable.value_or(false)) return false;
      PropertyDescriptor valueDesc;
      valueDesc.value = value;
      return r->DefineOwnProperty(key, valueDesc);
    }
    return CreateDataProperty(r, key, value);
  }
  Value setter = ownDesc->set.value_or(Value::undefined());
  if (setter.isUndefined()) return false;
  TRY(snowman::Call(setter, receiver, {value}));
  return true;
}

// 10.1.10.1 OrdinaryDelete
Result<bool> Object::Delete(const PropertyKey& key) {
  auto desc = TRY(GetOwnProperty(key));
  if (!desc) return true;
  if (desc->configurable.value_or(false)) {
    properties_->remove(key);
    return true;
  }
  return false;
}

// 10.1.11.1 OrdinaryOwnPropertyKeys
Result<std::vector<PropertyKey>> Object::OwnPropertyKeys() { return properties_->keys(); }

Result<Value> Object::Call(Value, Args) { return ThrowTypeError("not a function"); }

Result<Object*> Object::Construct(Args, Object*) { return ThrowTypeError("not a constructor"); }

// 10.1.12 OrdinaryObjectCreate
Object* OrdinaryObjectCreate(Object* prototype) { return make<Object>(prototype); }

// 10.1.6.2 IsCompatiblePropertyDescriptor
bool IsCompatiblePropertyDescriptor(bool extensible, const PropertyDescriptor& desc,
                                    const std::optional<PropertyDescriptor>& current) {
  return ValidateAndApplyPropertyDescriptor(nullptr, PropertyKey(u""), extensible, desc, current);
}

// 10.1.6.3 ValidateAndApplyPropertyDescriptor
bool ValidateAndApplyPropertyDescriptor(Object* object, const PropertyKey& key, bool extensible,
                                        const PropertyDescriptor& desc,
                                        const std::optional<PropertyDescriptor>& current) {
  if (!current) {
    if (!extensible) return false;
    if (!object) return true;
    Attr attrs = Attr::None;
    if (desc.enumerable.value_or(false)) attrs = attrs | Attr::Enumerable;
    if (desc.configurable.value_or(false)) attrs = attrs | Attr::Configurable;
    if (desc.isAccessorDescriptor()) {
      object->properties().put(key, PropertySlot::accessorPair(desc.get.value_or(Value::undefined()),
                                                               desc.set.value_or(Value::undefined()), attrs));
    } else {
      if (desc.writable.value_or(false)) attrs = attrs | Attr::Writable;
      object->properties().put(key, PropertySlot::data(desc.value.value_or(Value::undefined()), attrs));
    }
    return true;
  }
  if (!desc.value && !desc.get && !desc.set && !desc.writable && !desc.enumerable && !desc.configurable) return true;
  bool curConfigurable = current->configurable.value_or(false);
  bool curEnumerable = current->enumerable.value_or(false);
  if (!curConfigurable) {
    if (desc.configurable && *desc.configurable) return false;
    if (desc.enumerable && *desc.enumerable != curEnumerable) return false;
    if (!desc.isGenericDescriptor() && desc.isAccessorDescriptor() != current->isAccessorDescriptor()) return false;
    if (current->isAccessorDescriptor()) {
      if (desc.get && !SameValue(*desc.get, *current->get)) return false;
      if (desc.set && !SameValue(*desc.set, *current->set)) return false;
    } else if (!current->writable.value_or(false)) {
      if (desc.writable && *desc.writable) return false;
      if (desc.value && !SameValue(*desc.value, *current->value)) return false;
    }
  }
  if (object) {
    bool configurable = desc.configurable.value_or(curConfigurable);
    bool enumerable = desc.enumerable.value_or(curEnumerable);
    Attr attrs = Attr::None;
    if (enumerable) attrs = attrs | Attr::Enumerable;
    if (configurable) attrs = attrs | Attr::Configurable;
    if (current->isDataDescriptor() && desc.isAccessorDescriptor()) {
      object->properties().put(key, PropertySlot::accessorPair(desc.get.value_or(Value::undefined()),
                                                               desc.set.value_or(Value::undefined()), attrs));
    } else if (current->isAccessorDescriptor() && desc.isDataDescriptor()) {
      if (desc.writable.value_or(false)) attrs = attrs | Attr::Writable;
      object->properties().put(key, PropertySlot::data(desc.value.value_or(Value::undefined()), attrs));
    } else if (current->isAccessorDescriptor()) {
      object->properties().put(key, PropertySlot::accessorPair(desc.get.value_or(*current->get),
                                                               desc.set.value_or(*current->set), attrs));
    } else {
      if (desc.writable.value_or(current->writable.value_or(false))) attrs = attrs | Attr::Writable;
      object->properties().put(key, PropertySlot::data(desc.value.value_or(*current->value), attrs));
    }
  }
  return true;
}

// 6.2.6.6 CompletePropertyDescriptor
void CompletePropertyDescriptor(PropertyDescriptor& desc) {
  if (desc.isGenericDescriptor() || desc.isDataDescriptor()) {
    if (!desc.value) desc.value = Value::undefined();
    if (!desc.writable) desc.writable = false;
  } else {
    if (!desc.get) desc.get = Value::undefined();
    if (!desc.set) desc.set = Value::undefined();
  }
  if (!desc.enumerable) desc.enumerable = false;
  if (!desc.configurable) desc.configurable = false;
}

}  // namespace snowman
