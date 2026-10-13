// 10.4.6 Module Namespace Exotic Objects and GetModuleNamespace (16.2.1.11).
#include <algorithm>

#include "interp/module.h"
#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/operations.h"

namespace snowman::interp {

// 10.4.6.12 ModuleNamespaceCreate(module, exports)
ModuleNamespace::ModuleNamespace(ModuleRecord* module, std::vector<String*> exportNames)
    : Object(nullptr), module(module), exports(std::move(exportNames)) {
  std::sort(exports.begin(), exports.end(), [](String* a, String* b) { return a->view() < b->view(); });
  keys.reserve(exports.size());
  for (String* name : exports) keys.emplace_back(name);
  defineValue(this, PropertyKey(vm().symbols.toStringTag), Value(atom(u"Module")), Attr::None);
  extensible_ = false;
}

void ModuleNamespace::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(module);
  for (String* name : exports) tracer.visit(name);
}

int ModuleNamespace::find(const PropertyKey& key) const {
  for (size_t i = 0; i < keys.size(); i++) {
    if (keys[i] == key) return int(i);
  }
  return -1;
}

Result<Object*> ModuleNamespace::GetPrototypeOf() { return static_cast<Object*>(nullptr); }

// SetImmutablePrototype(O, V)
Result<bool> ModuleNamespace::SetPrototypeOf(Object* prototype) { return prototype == nullptr; }

// Always false once constructed (the constructor defines @@toStringTag first).
Result<bool> ModuleNamespace::IsExtensible() { return extensible_; }

Result<bool> ModuleNamespace::PreventExtensions() { return true; }

Result<std::optional<PropertyDescriptor>> ModuleNamespace::GetOwnProperty(const PropertyKey& key) {
  if (key.isSymbol()) return Object::GetOwnProperty(key);
  if (find(key) < 0) return std::optional<PropertyDescriptor>();
  Value value = TRY(Get(key, Value(this)));
  return std::optional<PropertyDescriptor>(PropertyDescriptor{value, {}, {}, true, true, false});
}

Result<bool> ModuleNamespace::DefineOwnProperty(const PropertyKey& key, const PropertyDescriptor& desc) {
  if (key.isSymbol()) return Object::DefineOwnProperty(key, desc);
  std::optional<PropertyDescriptor> current = TRY(GetOwnProperty(key));
  if (!current) return false;
  if (desc.configurable && *desc.configurable) return false;
  if (desc.enumerable && !*desc.enumerable) return false;
  if (desc.isAccessorDescriptor()) return false;
  if (desc.writable && !*desc.writable) return false;
  if (desc.value) return SameValue(*desc.value, *current->value);
  return true;
}

Result<bool> ModuleNamespace::HasProperty(const PropertyKey& key) {
  if (key.isSymbol()) return Object::HasProperty(key);
  return find(key) >= 0;
}

Result<Value> ModuleNamespace::Get(const PropertyKey& key, Value receiver) {
  if (key.isSymbol()) return Object::Get(key, receiver);
  int index = find(key);
  if (index < 0) return Value::undefined();
  ResolvedBinding binding = ResolveExport(module, exports[size_t(index)]);
  ModuleRecord* targetModule = binding.module;
  if (!binding.bindingName) return Value(GetModuleNamespace(targetModule));
  ModuleEnvironment* targetEnv = targetModule->environment;
  if (!targetEnv) {
    return ThrowReferenceError("Cannot access '" + binding.bindingName->toUtf8() + "' before initialization");
  }
  return targetEnv->GetBindingValue(binding.bindingName, true);
}

Result<bool> ModuleNamespace::Set(const PropertyKey&, Value, Value) { return false; }

Result<bool> ModuleNamespace::Delete(const PropertyKey& key) {
  if (key.isSymbol()) return Object::Delete(key);
  return find(key) < 0;
}

Result<std::vector<PropertyKey>> ModuleNamespace::OwnPropertyKeys() {
  std::vector<PropertyKey> result = keys;
  std::vector<PropertyKey> symbolKeys = TRY(Object::OwnPropertyKeys());
  for (const PropertyKey& key : symbolKeys) result.push_back(key);
  return result;
}

}  // namespace snowman::interp
