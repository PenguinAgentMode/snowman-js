// The Reflect object (28.1).
#include "builtins/util.h"

namespace snowman::builtins {

namespace {

Result<Object*> requireTarget(Value target) {
  if (!target.isObject()) return ThrowTypeError("Reflect method called on non-object");
  return target.asObject();
}

Result<Value> Reflect_apply(NativeCall& call) {
  Value target = call.args[0];
  if (!IsCallable(target)) return ThrowTypeError("Reflect.apply: target is not a function");
  ValueVector args = TRY(CreateListFromArrayLike(call.args[2]));
  return Call(target, call.args[1], args);
}

Result<Value> Reflect_construct(NativeCall& call) {
  Value target = call.args[0];
  if (!IsConstructor(target)) return ThrowTypeError("Reflect.construct: target is not a constructor");
  Value newTarget = call.args.size() < 3 ? target : call.args[2];
  if (!IsConstructor(newTarget)) return ThrowTypeError("Reflect.construct: newTarget is not a constructor");
  ValueVector args = TRY(CreateListFromArrayLike(call.args[1]));
  return Value(TRY(Construct(target.asObject(), args, newTarget.asObject())));
}

Result<Value> Reflect_defineProperty(NativeCall& call) {
  Object* target = TRY(requireTarget(call.args[0]));
  PropertyKey key = TRY(ToPropertyKey(call.args[1]));
  PropertyDescriptor desc = TRY(ToPropertyDescriptor(call.args[2]));
  return boolean(TRY(target->DefineOwnProperty(key, desc)));
}

Result<Value> Reflect_deleteProperty(NativeCall& call) {
  Object* target = TRY(requireTarget(call.args[0]));
  PropertyKey key = TRY(ToPropertyKey(call.args[1]));
  return boolean(TRY(target->Delete(key)));
}

Result<Value> Reflect_get(NativeCall& call) {
  Object* target = TRY(requireTarget(call.args[0]));
  PropertyKey key = TRY(ToPropertyKey(call.args[1]));
  Value receiver = call.args.size() < 3 ? Value(target) : call.args[2];
  return target->Get(key, receiver);
}

Result<Value> Reflect_getOwnPropertyDescriptor(NativeCall& call) {
  Object* target = TRY(requireTarget(call.args[0]));
  PropertyKey key = TRY(ToPropertyKey(call.args[1]));
  auto desc = TRY(target->GetOwnProperty(key));
  return FromPropertyDescriptor(desc);
}

Result<Value> Reflect_getPrototypeOf(NativeCall& call) {
  Object* target = TRY(requireTarget(call.args[0]));
  Object* proto = TRY(target->GetPrototypeOf());
  return proto ? Value(proto) : Value::null();
}

Result<Value> Reflect_has(NativeCall& call) {
  Object* target = TRY(requireTarget(call.args[0]));
  PropertyKey key = TRY(ToPropertyKey(call.args[1]));
  return boolean(TRY(target->HasProperty(key)));
}

Result<Value> Reflect_isExtensible(NativeCall& call) {
  Object* target = TRY(requireTarget(call.args[0]));
  return boolean(TRY(target->IsExtensible()));
}

Result<Value> Reflect_ownKeys(NativeCall& call) {
  Object* target = TRY(requireTarget(call.args[0]));
  std::vector<PropertyKey> keys = TRY(target->OwnPropertyKeys());
  ValueVector list;
  for (const PropertyKey& k : keys) list.push_back(k.toValue());
  return Value(CreateArrayFromList(list));
}

Result<Value> Reflect_preventExtensions(NativeCall& call) {
  Object* target = TRY(requireTarget(call.args[0]));
  return boolean(TRY(target->PreventExtensions()));
}

Result<Value> Reflect_set(NativeCall& call) {
  Object* target = TRY(requireTarget(call.args[0]));
  PropertyKey key = TRY(ToPropertyKey(call.args[1]));
  Value receiver = call.args.size() < 4 ? Value(target) : call.args[3];
  return boolean(TRY(target->Set(key, call.args[2], receiver)));
}

Result<Value> Reflect_setPrototypeOf(NativeCall& call) {
  Object* target = TRY(requireTarget(call.args[0]));
  Value proto = call.args[1];
  if (!proto.isObject() && !proto.isNull()) return ThrowTypeError("Object prototype may only be an Object or null");
  return boolean(TRY(target->SetPrototypeOf(proto.isNull() ? nullptr : proto.asObject())));
}

void installReflect(Realm& realm) {
  Object* reflect = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  realm.setIntrinsic(Intrinsic::Reflect, reflect);
  defineGlobal(realm, PropertyKey(u"Reflect"), reflect);
  defineFunction(realm, reflect, PropertyKey(u"apply"), 3, Reflect_apply);
  defineFunction(realm, reflect, PropertyKey(u"construct"), 2, Reflect_construct);
  defineFunction(realm, reflect, PropertyKey(u"defineProperty"), 3, Reflect_defineProperty);
  defineFunction(realm, reflect, PropertyKey(u"deleteProperty"), 2, Reflect_deleteProperty);
  defineFunction(realm, reflect, PropertyKey(u"get"), 2, Reflect_get);
  defineFunction(realm, reflect, PropertyKey(u"getOwnPropertyDescriptor"), 2, Reflect_getOwnPropertyDescriptor);
  defineFunction(realm, reflect, PropertyKey(u"getPrototypeOf"), 1, Reflect_getPrototypeOf);
  defineFunction(realm, reflect, PropertyKey(u"has"), 2, Reflect_has);
  defineFunction(realm, reflect, PropertyKey(u"isExtensible"), 1, Reflect_isExtensible);
  defineFunction(realm, reflect, PropertyKey(u"ownKeys"), 1, Reflect_ownKeys);
  defineFunction(realm, reflect, PropertyKey(u"preventExtensions"), 1, Reflect_preventExtensions);
  defineFunction(realm, reflect, PropertyKey(u"set"), 3, Reflect_set);
  defineFunction(realm, reflect, PropertyKey(u"setPrototypeOf"), 2, Reflect_setPrototypeOf);
  defineValue(reflect, PropertyKey(vm().symbols.toStringTag), str(u"Reflect"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(Reflect, kOrderDefault, installReflect);

}  // namespace snowman::builtins
