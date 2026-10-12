// Object (20.1): the constructor, its statics, and Object.prototype (with Annex B's __proto__ and
// __defineGetter__ family).
#include "builtins/util.h"
#include "runtime/array.h"
#include "runtime/function.h"
#include "runtime/wrappers.h"

namespace snowman::builtins {

namespace {

// 20.1.1.1 Object ( [ value ] )
Result<Value> Object_construct(NativeCall& call) {
  if (call.newTarget && call.newTarget != call.callee) {
    return Value(TRY(OrdinaryCreateFromConstructor<Object>(call.newTarget, Intrinsic::ObjectPrototype)));
  }
  Value value = call.args[0];
  if (value.isNullish()) return Value(newPlainObject());
  return Value(TRY(ToObject(value)));
}

// 20.1.2.1 Object.assign ( target, ...sources )
Result<Value> Object_assign(NativeCall& call) {
  Object* to = TRY(ToObject(call.args[0]));
  for (size_t i = 1; i < call.args.size(); i++) {
    Value nextSource = call.args[i];
    if (nextSource.isNullish()) continue;
    Object* from = TRY(ToObject(nextSource));
    std::vector<PropertyKey> keys = TRY(from->OwnPropertyKeys());
    RootedKeys rootedKeys(keys);
    for (const PropertyKey& key : keys) {
      auto desc = TRY(from->GetOwnProperty(key));
      if (desc && *desc->enumerable) {
        Value propValue = TRY(Get(from, key));
        TRY(Set(to, key, propValue, true));
      }
    }
  }
  return Value(to);
}

// 20.1.2.3.1 ObjectDefineProperties
Result<Object*> ObjectDefineProperties(Object* object, Value properties) {
  Object* props = TRY(ToObject(properties));
  std::vector<PropertyKey> keys = TRY(props->OwnPropertyKeys());
  RootedKeys rootedKeys(keys);
  std::vector<std::pair<PropertyKey, PropertyDescriptor>> descriptors;
  ValueVector descriptorValues;  // keeps the descriptors' values and accessors alive
  for (const PropertyKey& nextKey : keys) {
    auto propDesc = TRY(props->GetOwnProperty(nextKey));
    if (propDesc && *propDesc->enumerable) {
      Value descObj = TRY(Get(props, nextKey));
      PropertyDescriptor desc = TRY(ToPropertyDescriptor(descObj));
      descriptorValues.push_back(desc.value.value_or(Value()));
      descriptorValues.push_back(desc.get.value_or(Value()));
      descriptorValues.push_back(desc.set.value_or(Value()));
      descriptors.emplace_back(nextKey, desc);
    }
  }
  for (auto& [key, desc] : descriptors) TRY(DefinePropertyOrThrow(object, key, desc));
  return object;
}

// 20.1.2.2 Object.create ( O, Properties )
Result<Value> Object_create(NativeCall& call) {
  Value o = call.args[0];
  if (!o.isObject() && !o.isNull()) return ThrowTypeError("Object prototype may only be an Object or null");
  Object* obj = OrdinaryObjectCreate(o.isNull() ? nullptr : o.asObject());
  if (!call.args[1].isUndefined()) return Value(TRY(ObjectDefineProperties(obj, call.args[1])));
  return Value(obj);
}

// 20.1.2.3 Object.defineProperties ( O, Properties )
Result<Value> Object_defineProperties(NativeCall& call) {
  if (!call.args[0].isObject()) return ThrowTypeError("Object.defineProperties called on non-object");
  return Value(TRY(ObjectDefineProperties(call.args[0].asObject(), call.args[1])));
}

// 20.1.2.4 Object.defineProperty ( O, P, Attributes )
Result<Value> Object_defineProperty(NativeCall& call) {
  if (!call.args[0].isObject()) return ThrowTypeError("Object.defineProperty called on non-object");
  PropertyKey key = TRY(ToPropertyKey(call.args[1]));
  PropertyDescriptor desc = TRY(ToPropertyDescriptor(call.args[2]));
  TRY(DefinePropertyOrThrow(call.args[0].asObject(), key, desc));
  return call.args[0];
}

Result<Value> enumerableOwn(NativeCall& call, EnumerableOwnKind kind) {
  Object* obj = TRY(ToObject(call.args[0]));
  ValueVector list = TRY(EnumerableOwnProperties(obj, kind));
  return Value(CreateArrayFromList(list));
}

Result<Value> Object_entries(NativeCall& call) { return enumerableOwn(call, EnumerableOwnKind::KeyValue); }
Result<Value> Object_keys(NativeCall& call) { return enumerableOwn(call, EnumerableOwnKind::Key); }
Result<Value> Object_values(NativeCall& call) { return enumerableOwn(call, EnumerableOwnKind::Value); }

// 20.1.2.6 Object.freeze ( O ), 20.1.2.22 Object.seal ( O )
Result<Value> Object_freeze(NativeCall& call) {
  Value o = call.args[0];
  if (!o.isObject()) return o;
  bool status = TRY(SetIntegrityLevel(o.asObject(), IntegrityLevel::Frozen));
  if (!status) return ThrowTypeError("Cannot freeze object");
  return o;
}

Result<Value> Object_seal(NativeCall& call) {
  Value o = call.args[0];
  if (!o.isObject()) return o;
  bool status = TRY(SetIntegrityLevel(o.asObject(), IntegrityLevel::Sealed));
  if (!status) return ThrowTypeError("Cannot seal object");
  return o;
}

// 20.1.2.7 Object.fromEntries ( iterable )
Result<Value> Object_fromEntries(NativeCall& call) {
  TRY(RequireObjectCoercible(call.args[0]));
  Object* obj = newPlainObject();
  IteratorRecord record = TRY(GetIterator(call.args[0], IteratorKind::Sync));
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(record));
    if (!next) return Value(obj);
    if (!next->isObject()) return closeIteratorWithThrow(record, ThrowTypeError("Iterator value is not an entry object").value);
    Value k = TRY_CLOSE(record, Get(next->asObject(), PropertyKey::index(0)));
    Value v = TRY_CLOSE(record, Get(next->asObject(), PropertyKey::index(1)));
    PropertyKey key = TRY_CLOSE(record, ToPropertyKey(k));
    TRY_CLOSE(record, CreateDataPropertyOrThrow(obj, key, v));
  }
}

// 20.1.2.8 Object.getOwnPropertyDescriptor ( O, P )
Result<Value> Object_getOwnPropertyDescriptor(NativeCall& call) {
  Object* obj = TRY(ToObject(call.args[0]));
  PropertyKey key = TRY(ToPropertyKey(call.args[1]));
  auto desc = TRY(obj->GetOwnProperty(key));
  return FromPropertyDescriptor(desc);
}

// 20.1.2.9 Object.getOwnPropertyDescriptors ( O )
Result<Value> Object_getOwnPropertyDescriptors(NativeCall& call) {
  Object* obj = TRY(ToObject(call.args[0]));
  std::vector<PropertyKey> ownKeys = TRY(obj->OwnPropertyKeys());
  RootedKeys rootedKeys(ownKeys);
  Object* descriptors = newPlainObject();
  for (const PropertyKey& key : ownKeys) {
    auto desc = TRY(obj->GetOwnProperty(key));
    Value descriptor = FromPropertyDescriptor(desc);
    if (!descriptor.isUndefined()) TRY(CreateDataPropertyOrThrow(descriptors, key, descriptor));
  }
  return Value(descriptors);
}

// 20.1.2.11.1 GetOwnPropertyKeys
Result<Value> GetOwnPropertyKeys(Value o, bool symbols) {
  Object* obj = TRY(ToObject(o));
  std::vector<PropertyKey> keys = TRY(obj->OwnPropertyKeys());
  ValueVector nameList;
  for (const PropertyKey& key : keys) {
    if (key.isSymbol() == symbols) nameList.push_back(key.toValue());
  }
  return Value(CreateArrayFromList(nameList));
}

Result<Value> Object_getOwnPropertyNames(NativeCall& call) { return GetOwnPropertyKeys(call.args[0], false); }
Result<Value> Object_getOwnPropertySymbols(NativeCall& call) { return GetOwnPropertyKeys(call.args[0], true); }

// 20.1.2.12 Object.getPrototypeOf ( O )
Result<Value> Object_getPrototypeOf(NativeCall& call) {
  Object* obj = TRY(ToObject(call.args[0]));
  Object* proto = TRY(obj->GetPrototypeOf());
  return proto ? Value(proto) : Value::null();
}

// 20.1.2.13 Object.groupBy ( items, callback )
Result<Value> Object_groupBy(NativeCall& call) {
  Value items = call.args[0], callback = call.args[1];
  TRY(RequireObjectCoercible(items));
  if (!IsCallable(callback)) return ThrowTypeError("Object.groupBy: callback is not a function");
  std::vector<std::pair<PropertyKey, ValueVector>> groups;
  ValueVector groupKeys;  // keeps Symbol keys alive
  IteratorRecord record = TRY(GetIterator(items, IteratorKind::Sync));
  double k = 0;
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(record));
    if (!next) break;
    Value keyValue = TRY_CLOSE(record, Call(callback, Value::undefined(), {*next, Value::number(k)}));
    PropertyKey key = TRY_CLOSE(record, ToPropertyKey(keyValue));
    bool found = false;
    for (auto& [gk, elements] : groups) {
      if (gk == key) {
        elements.push_back(*next);
        found = true;
        break;
      }
    }
    if (!found) {
      groupKeys.push_back(key.toValue());
      groups.emplace_back(key, ValueVector{});
      groups.back().second.push_back(*next);
    }
    k++;
  }
  Object* obj = OrdinaryObjectCreate(nullptr);
  for (auto& [key, elements] : groups) TRY(CreateDataPropertyOrThrow(obj, key, CreateArrayFromList(elements)));
  return Value(obj);
}

// 20.1.2.14 Object.hasOwn ( O, P )
Result<Value> Object_hasOwn(NativeCall& call) {
  Object* obj = TRY(ToObject(call.args[0]));
  PropertyKey key = TRY(ToPropertyKey(call.args[1]));
  return Value::boolean(TRY(HasOwnProperty(obj, key)));
}

// 20.1.2.15 Object.is ( value1, value2 )
Result<Value> Object_is(NativeCall& call) { return Value::boolean(SameValue(call.args[0], call.args[1])); }

// 20.1.2.16 Object.isExtensible ( O )
Result<Value> Object_isExtensible(NativeCall& call) {
  if (!call.args[0].isObject()) return Value::boolean(false);
  return Value::boolean(TRY(IsExtensible(call.args[0].asObject())));
}

// 20.1.2.17 Object.isFrozen ( O ), 20.1.2.18 Object.isSealed ( O )
Result<Value> Object_isFrozen(NativeCall& call) {
  if (!call.args[0].isObject()) return Value::boolean(true);
  return Value::boolean(TRY(TestIntegrityLevel(call.args[0].asObject(), IntegrityLevel::Frozen)));
}

Result<Value> Object_isSealed(NativeCall& call) {
  if (!call.args[0].isObject()) return Value::boolean(true);
  return Value::boolean(TRY(TestIntegrityLevel(call.args[0].asObject(), IntegrityLevel::Sealed)));
}

// 20.1.2.20 Object.preventExtensions ( O )
Result<Value> Object_preventExtensions(NativeCall& call) {
  Value o = call.args[0];
  if (!o.isObject()) return o;
  bool status = TRY(o.asObject()->PreventExtensions());
  if (!status) return ThrowTypeError("Cannot prevent extensions");
  return o;
}

// 20.1.2.23 Object.setPrototypeOf ( O, proto )
Result<Value> Object_setPrototypeOf(NativeCall& call) {
  Value o = call.args[0], proto = call.args[1];
  TRY(RequireObjectCoercible(o));
  if (!proto.isObject() && !proto.isNull()) return ThrowTypeError("Object prototype may only be an Object or null");
  if (!o.isObject()) return o;
  bool status = TRY(o.asObject()->SetPrototypeOf(proto.isNull() ? nullptr : proto.asObject()));
  if (!status) return ThrowTypeError("Cannot set prototype");
  return o;
}

// 20.1.3.2 Object.prototype.hasOwnProperty ( V )
Result<Value> ObjectPrototype_hasOwnProperty(NativeCall& call) {
  PropertyKey key = TRY(ToPropertyKey(call.args[0]));
  Object* o = TRY(ToObject(call.thisValue));
  return Value::boolean(TRY(HasOwnProperty(o, key)));
}

// 20.1.3.3 Object.prototype.isPrototypeOf ( V )
Result<Value> ObjectPrototype_isPrototypeOf(NativeCall& call) {
  Value v = call.args[0];
  if (!v.isObject()) return Value::boolean(false);
  Object* o = TRY(ToObject(call.thisValue));
  Object* p = v.asObject();
  while (true) {
    p = TRY(p->GetPrototypeOf());
    if (!p) return Value::boolean(false);
    if (p == o) return Value::boolean(true);
  }
}

// 20.1.3.4 Object.prototype.propertyIsEnumerable ( V )
Result<Value> ObjectPrototype_propertyIsEnumerable(NativeCall& call) {
  PropertyKey key = TRY(ToPropertyKey(call.args[0]));
  Object* o = TRY(ToObject(call.thisValue));
  auto desc = TRY(o->GetOwnProperty(key));
  if (!desc) return Value::boolean(false);
  return Value::boolean(*desc->enumerable);
}

// 20.1.3.5 Object.prototype.toLocaleString ( [ reserved1 [ , reserved2 ] ] )
Result<Value> ObjectPrototype_toLocaleString(NativeCall& call) {
  return Invoke(call.thisValue, PropertyKey(u"toString"));
}

}  // namespace

// 20.1.3.6 Object.prototype.toString ( )
Result<Value> ObjectPrototype_toString(NativeCall& call) {
  Value thisValue = call.thisValue;
  if (thisValue.isUndefined()) return str(u"[object Undefined]");
  if (thisValue.isNull()) return str(u"[object Null]");
  Object* o = TRY(ToObject(thisValue));
  bool isArray = TRY(IsArray(o));
  const char16_t* builtinTag;
  if (isArray) builtinTag = u"Array";
  else if (o->is<ArgumentsObject>() || o->hasParameterMap()) builtinTag = u"Arguments";
  else if (o->isCallable()) builtinTag = u"Function";
  else if (o->is<ErrorObject>()) builtinTag = u"Error";
  else if (o->is<BooleanObject>()) builtinTag = u"Boolean";
  else if (o->is<NumberObject>()) builtinTag = u"Number";
  else if (o->is<StringObject>()) builtinTag = u"String";
  else if (o->hasDateValue()) builtinTag = u"Date";
  else if (IsRegExpLike(o)) builtinTag = u"RegExp";
  else builtinTag = u"Object";
  Value tag = TRY(Get(o, PropertyKey(symbols().toStringTag)));
  std::u16string_view tagView = tag.isString() ? tag.asString()->view() : std::u16string_view(builtinTag);
  return Value(concatStrings({u"[object ", tagView, u"]"}));
}

namespace {

// 20.1.3.7 Object.prototype.valueOf ( )
Result<Value> ObjectPrototype_valueOf(NativeCall& call) { return Value(TRY(ToObject(call.thisValue))); }

// 20.1.3.8.1 get Object.prototype.__proto__
Result<Value> ObjectPrototype_getProto(NativeCall& call) {
  Object* o = TRY(ToObject(call.thisValue));
  Object* proto = TRY(o->GetPrototypeOf());
  return proto ? Value(proto) : Value::null();
}

// 20.1.3.8.2 set Object.prototype.__proto__
Result<Value> ObjectPrototype_setProto(NativeCall& call) {
  Value o = call.thisValue, proto = call.args[0];
  TRY(RequireObjectCoercible(o));
  if (!proto.isObject() && !proto.isNull()) return Value::undefined();
  if (!o.isObject()) return Value::undefined();
  bool status = TRY(o.asObject()->SetPrototypeOf(proto.isNull() ? nullptr : proto.asObject()));
  if (!status) return ThrowTypeError("Object.prototype.__proto__: cannot set prototype");
  return Value::undefined();
}

// B.2.2.2 / B.2.2.3 __defineGetter__ / __defineSetter__
Result<Value> defineAccessorAnnexB(NativeCall& call, bool getter) {
  Object* o = TRY(ToObject(call.thisValue));
  if (!IsCallable(call.args[1])) return ThrowTypeError("accessor is not a function");
  PropertyDescriptor desc;
  if (getter) desc.get = call.args[1];
  else desc.set = call.args[1];
  desc.enumerable = true;
  desc.configurable = true;
  PropertyKey key = TRY(ToPropertyKey(call.args[0]));
  TRY(DefinePropertyOrThrow(o, key, desc));
  return Value::undefined();
}

Result<Value> ObjectPrototype_defineGetter(NativeCall& call) { return defineAccessorAnnexB(call, true); }
Result<Value> ObjectPrototype_defineSetter(NativeCall& call) { return defineAccessorAnnexB(call, false); }

// B.2.2.4 / B.2.2.5 __lookupGetter__ / __lookupSetter__
Result<Value> lookupAccessorAnnexB(NativeCall& call, bool getter) {
  Object* o = TRY(ToObject(call.thisValue));
  PropertyKey key = TRY(ToPropertyKey(call.args[0]));
  while (o) {
    auto desc = TRY(o->GetOwnProperty(key));
    if (desc) {
      if (desc->isAccessorDescriptor()) return getter ? *desc->get : *desc->set;
      return Value::undefined();
    }
    o = TRY(o->GetPrototypeOf());
  }
  return Value::undefined();
}

Result<Value> ObjectPrototype_lookupGetter(NativeCall& call) { return lookupAccessorAnnexB(call, true); }
Result<Value> ObjectPrototype_lookupSetter(NativeCall& call) { return lookupAccessorAnnexB(call, false); }

void installObject(Realm& realm) {
  Object* proto = realm.intrinsic(Intrinsic::ObjectPrototype);
  BuiltinFunction* ctor =
      defineConstructor(realm, Intrinsic::Object, Object_construct, 1, PropertyKey(u"Object"), proto, Intrinsic::ObjectPrototype);
  defineFunction(realm, ctor, PropertyKey(u"assign"), 2, Object_assign);
  defineFunction(realm, ctor, PropertyKey(u"create"), 2, Object_create);
  defineFunction(realm, ctor, PropertyKey(u"defineProperties"), 2, Object_defineProperties);
  defineFunction(realm, ctor, PropertyKey(u"defineProperty"), 3, Object_defineProperty);
  defineFunction(realm, ctor, PropertyKey(u"entries"), 1, Object_entries);
  defineFunction(realm, ctor, PropertyKey(u"freeze"), 1, Object_freeze);
  defineFunction(realm, ctor, PropertyKey(u"fromEntries"), 1, Object_fromEntries);
  defineFunction(realm, ctor, PropertyKey(u"getOwnPropertyDescriptor"), 2, Object_getOwnPropertyDescriptor);
  defineFunction(realm, ctor, PropertyKey(u"getOwnPropertyDescriptors"), 1, Object_getOwnPropertyDescriptors);
  defineFunction(realm, ctor, PropertyKey(u"getOwnPropertyNames"), 1, Object_getOwnPropertyNames);
  defineFunction(realm, ctor, PropertyKey(u"getOwnPropertySymbols"), 1, Object_getOwnPropertySymbols);
  defineFunction(realm, ctor, PropertyKey(u"getPrototypeOf"), 1, Object_getPrototypeOf);
  defineFunction(realm, ctor, PropertyKey(u"groupBy"), 2, Object_groupBy);
  defineFunction(realm, ctor, PropertyKey(u"hasOwn"), 2, Object_hasOwn);
  defineFunction(realm, ctor, PropertyKey(u"is"), 2, Object_is);
  defineFunction(realm, ctor, PropertyKey(u"isExtensible"), 1, Object_isExtensible);
  defineFunction(realm, ctor, PropertyKey(u"isFrozen"), 1, Object_isFrozen);
  defineFunction(realm, ctor, PropertyKey(u"isSealed"), 1, Object_isSealed);
  defineFunction(realm, ctor, PropertyKey(u"keys"), 1, Object_keys);
  defineFunction(realm, ctor, PropertyKey(u"preventExtensions"), 1, Object_preventExtensions);
  defineFunction(realm, ctor, PropertyKey(u"seal"), 1, Object_seal);
  defineFunction(realm, ctor, PropertyKey(u"setPrototypeOf"), 2, Object_setPrototypeOf);
  defineFunction(realm, ctor, PropertyKey(u"values"), 1, Object_values);

  defineFunction(realm, proto, PropertyKey(u"hasOwnProperty"), 1, ObjectPrototype_hasOwnProperty);
  defineFunction(realm, proto, PropertyKey(u"isPrototypeOf"), 1, ObjectPrototype_isPrototypeOf);
  defineFunction(realm, proto, PropertyKey(u"propertyIsEnumerable"), 1, ObjectPrototype_propertyIsEnumerable);
  defineFunction(realm, proto, PropertyKey(u"toLocaleString"), 0, ObjectPrototype_toLocaleString);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, ObjectPrototype_toString);
  defineFunction(realm, proto, PropertyKey(u"valueOf"), 0, ObjectPrototype_valueOf);
  defineAccessor(realm, proto, PropertyKey(u"__proto__"), ObjectPrototype_getProto, ObjectPrototype_setProto);
  defineFunction(realm, proto, PropertyKey(u"__defineGetter__"), 2, ObjectPrototype_defineGetter);
  defineFunction(realm, proto, PropertyKey(u"__defineSetter__"), 2, ObjectPrototype_defineSetter);
  defineFunction(realm, proto, PropertyKey(u"__lookupGetter__"), 1, ObjectPrototype_lookupGetter);
  defineFunction(realm, proto, PropertyKey(u"__lookupSetter__"), 1, ObjectPrototype_lookupSetter);
}

}  // namespace

SNOWMAN_BUILTIN(Object, kOrderFundamental, installObject);

}  // namespace snowman::builtins
