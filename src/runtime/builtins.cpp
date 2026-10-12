// Helpers for declaring built-in properties (INTERFACES.md section 9).
#include "runtime/builtins.h"

#include "runtime/operations.h"
#include "runtime/property_map.h"
#include "runtime/string.h"

namespace snowman {

BuiltinFunction* createBuiltinFunction(Realm& realm, NativeFn fn, int length, const PropertyKey& name,
                                       bool isConstructor, Object* prototype, std::u16string_view prefix) {
  BuiltinFunction* function = make<BuiltinFunction>(
      &realm, fn, prototype ? prototype : realm.intrinsic(Intrinsic::FunctionPrototype), isConstructor);
  function->properties().put(PropertyKey(u"length"), PropertySlot::data(Value::number(length), Attr::Configurable));
  String* functionName = FunctionNameFromKey(name, prefix);
  function->properties().put(PropertyKey(u"name"), PropertySlot::data(functionName, Attr::Configurable));
  function->initialName = functionName;
  return function;
}

BuiltinFunction* defineFunction(Realm& realm, Object* target, const PropertyKey& name, int length, NativeFn fn,
                                Attr attrs) {
  BuiltinFunction* function = createBuiltinFunction(realm, fn, length, name);
  (void)target->DefineOwnProperty(name, PropertyDescriptor::data(function, attrs));
  return function;
}

void defineAccessor(Realm& realm, Object* target, const PropertyKey& name, NativeFn getter, NativeFn setter,
                    Attr attrs) {
  Value get = getter ? Value(createBuiltinFunction(realm, getter, 0, name, false, nullptr, u"get")) : Value();
  Value set = setter ? Value(createBuiltinFunction(realm, setter, 1, name, false, nullptr, u"set")) : Value();
  (void)target->DefineOwnProperty(name, PropertyDescriptor::accessor(get, set, attrs));
}

void defineValue(Object* target, const PropertyKey& name, Value value, Attr attrs) {
  (void)target->DefineOwnProperty(name, PropertyDescriptor::data(value, attrs));
}

void defineGlobal(Realm& realm, const PropertyKey& name, Value value) {
  defineValue(realm.globalObject(), name, value, Attr::WritableConfigurable);
}

BuiltinFunction* defineConstructor(Realm& realm, Intrinsic constructorId, NativeFn fn, int length,
                                   const PropertyKey& name, Object* prototypeObject, Intrinsic prototypeId,
                                   bool defineGlobalBinding) {
  BuiltinFunction* constructor = createBuiltinFunction(realm, fn, length, name, true);
  defineValue(constructor, PropertyKey(u"prototype"), prototypeObject, Attr::None);
  defineValue(prototypeObject, PropertyKey(u"constructor"), constructor, Attr::WritableConfigurable);
  realm.setIntrinsic(constructorId, constructor);
  realm.setIntrinsic(prototypeId, prototypeObject);
  if (defineGlobalBinding) defineGlobal(realm, name, constructor);
  return constructor;
}

}  // namespace snowman
