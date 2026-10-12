// 7.2 Testing and comparison, 7.3 Operations on objects, 10.1 helpers.
#include "runtime/operations.h"

#include <cmath>

#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/bound_function.h"
#include "runtime/property_map.h"
#include "runtime/proxy.h"
#include "runtime/symbol.h"
#include "runtime/vm.h"

namespace snowman {

// 7.2.1 RequireObjectCoercible
Result<void> RequireObjectCoercible(Value argument) {
  if (argument.isNullish()) return ThrowTypeError("Cannot convert undefined or null to object");
  return {};
}

// 7.2.2 IsArray
Result<bool> IsArray(Value argument) {
  if (!argument.isObject()) return false;
  Object* object = argument.asObject();
  if (object->is<ArrayObject>()) return true;
  if (auto* proxy = object->as<ProxyObject>()) {
    if (!proxy->proxyHandler) return ThrowTypeError("Cannot perform 'IsArray' on a proxy that has been revoked");
    return IsArray(proxy->proxyTarget);
  }
  return false;
}

// 7.2.3 IsCallable, 7.2.4 IsConstructor
bool IsCallable(Value argument) { return argument.isObject() && argument.asObject()->isCallable(); }
bool IsConstructor(Value argument) { return argument.isObject() && argument.asObject()->isConstructor(); }

// 7.2.6 IsIntegralNumber
bool IsIntegralNumber(Value argument) {
  if (!argument.isNumber()) return false;
  double d = argument.asNumber();
  return std::isfinite(d) && std::trunc(d) == d;
}

// 7.2.8 IsRegExp
Result<bool> IsRegExp(Value argument) {
  if (!argument.isObject()) return false;
  Value matcher = TRY(Get(argument.asObject(), PropertyKey(vm().symbols.match)));
  if (!matcher.isUndefined()) return ToBoolean(matcher);
  return IsRegExpLike(argument);
}

namespace {
RegExpCheck& regExpCheck() {
  static RegExpCheck check = nullptr;
  return check;
}
}  // namespace

void setRegExpCheck(RegExpCheck check) { regExpCheck() = check; }

bool IsRegExpLike(Value argument) {
  if (!argument.isObject()) return false;
  RegExpCheck check = regExpCheck();
  return check && check(argument.asObject());
}

// 7.2.9 SameValue, 7.2.10 SameValueZero, 7.2.11 SameValueNonNumber
bool SameValue(Value x, Value y) {
  if (x.isNumber() && y.isNumber()) {
    double a = x.asNumber(), b = y.asNumber();
    if (std::isnan(a) && std::isnan(b)) return true;
    if (a == 0 && b == 0) return std::signbit(a) == std::signbit(b);
    return a == b;
  }
  return SameValueNonNumber(x, y);
}

bool SameValueZero(Value x, Value y) {
  if (x.isNumber() && y.isNumber()) {
    double a = x.asNumber(), b = y.asNumber();
    if (std::isnan(a) && std::isnan(b)) return true;
    return a == b;
  }
  return SameValueNonNumber(x, y);
}

bool SameValueNonNumber(Value x, Value y) {
  if (x.type() != y.type()) return false;
  switch (x.type()) {
    case ValueType::Undefined:
    case ValueType::Null:
    case ValueType::Empty:
      return true;
    case ValueType::Boolean:
      return x.asBoolean() == y.asBoolean();
    case ValueType::Number:
      return x.asNumber() == y.asNumber();
    case ValueType::String:
      return equals(x.asString(), y.asString());
    case ValueType::BigInt:
      return BigInt::equals(x.asBigInt(), y.asBigInt());
    case ValueType::Symbol:
    case ValueType::Object:
      return x.asCell() == y.asCell();
  }
  return false;
}

// 7.2.15 IsStrictlyEqual
bool IsStrictlyEqual(Value x, Value y) {
  if (x.type() != y.type()) return false;
  if (x.isNumber()) return x.asNumber() == y.asNumber();
  return SameValueNonNumber(x, y);
}

namespace {

std::optional<BigInt*> stringToBigInt(String* s) { return BigInt::fromString(s->view()); }

}  // namespace

// 7.2.14 IsLooselyEqual
Result<bool> IsLooselyEqual(Value x, Value y) {
  if (x.type() == y.type()) return IsStrictlyEqual(x, y);
  if (x.isNullish() && y.isNullish()) return true;
  if (x.isNumber() && y.isString()) return x.asNumber() == StringToNumber(y.asString()->view());
  if (x.isString() && y.isNumber()) return StringToNumber(x.asString()->view()) == y.asNumber();
  if (x.isBigInt() && y.isString()) {
    auto n = stringToBigInt(y.asString());
    if (!n) return false;
    return BigInt::equals(x.asBigInt(), *n);
  }
  if (x.isString() && y.isBigInt()) return IsLooselyEqual(y, x);
  if (x.isBoolean()) return IsLooselyEqual(Value::number(x.asBoolean() ? 1 : 0), y);
  if (y.isBoolean()) return IsLooselyEqual(x, Value::number(y.asBoolean() ? 1 : 0));
  if ((x.isString() || x.isNumber() || x.isBigInt() || x.isSymbol()) && y.isObject()) {
    Value prim = TRY(ToPrimitive(y));
    return IsLooselyEqual(x, prim);
  }
  if (x.isObject() && (y.isString() || y.isNumber() || y.isBigInt() || y.isSymbol())) {
    Value prim = TRY(ToPrimitive(x));
    return IsLooselyEqual(prim, y);
  }
  if ((x.isBigInt() && y.isNumber()) || (x.isNumber() && y.isBigInt())) {
    BigInt* b = x.isBigInt() ? x.asBigInt() : y.asBigInt();
    double d = x.isNumber() ? x.asNumber() : y.asNumber();
    if (!std::isfinite(d)) return false;
    auto c = BigInt::compareToNumber(b, d);
    return c && *c == 0;
  }
  return false;
}

// 7.2.13 IsLessThan
Result<std::optional<bool>> IsLessThan(Value x, Value y, bool leftFirst) {
  Value px, py;
  if (leftFirst) {
    px = TRY(ToPrimitive(x, PreferredType::Number));
    py = TRY(ToPrimitive(y, PreferredType::Number));
  } else {
    py = TRY(ToPrimitive(y, PreferredType::Number));
    px = TRY(ToPrimitive(x, PreferredType::Number));
  }
  if (px.isString() && py.isString()) {
    return std::optional<bool>(px.asString()->view() < py.asString()->view());
  }
  if (px.isBigInt() && py.isString()) {
    auto ny = stringToBigInt(py.asString());
    if (!ny) return std::optional<bool>();
    return std::optional<bool>(BigInt::compare(px.asBigInt(), *ny) < 0);
  }
  if (px.isString() && py.isBigInt()) {
    auto nx = stringToBigInt(px.asString());
    if (!nx) return std::optional<bool>();
    return std::optional<bool>(BigInt::compare(*nx, py.asBigInt()) < 0);
  }
  Value nx = TRY(ToNumeric(px));
  Value ny = TRY(ToNumeric(py));
  if (nx.isNumber() && ny.isNumber()) {
    double a = nx.asNumber(), b = ny.asNumber();
    if (std::isnan(a) || std::isnan(b)) return std::optional<bool>();
    return std::optional<bool>(a < b);
  }
  if (nx.isBigInt() && ny.isBigInt()) return std::optional<bool>(BigInt::compare(nx.asBigInt(), ny.asBigInt()) < 0);
  if (nx.isBigInt()) {
    auto c = BigInt::compareToNumber(nx.asBigInt(), ny.asNumber());
    if (!c) return std::optional<bool>();
    return std::optional<bool>(*c < 0);
  }
  auto c = BigInt::compareToNumber(ny.asBigInt(), nx.asNumber());
  if (!c) return std::optional<bool>();
  return std::optional<bool>(*c > 0);
}

// 7.3 Operations on objects
Result<bool> IsExtensible(Object* object) { return object->IsExtensible(); }

Result<Value> Get(Object* object, const PropertyKey& key) { return object->Get(key, object); }

Result<Value> GetV(Value value, const PropertyKey& key) {
  if (value.isObject()) return value.asObject()->Get(key, value);
  Object* object = TRY(ToObject(value));
  return object->Get(key, value);
}

Result<void> Set(Object* object, const PropertyKey& key, Value value, bool throwOnFailure) {
  bool success = TRY(object->Set(key, value, object));
  if (!success && throwOnFailure) return ThrowTypeError("Cannot assign to read only property");
  return {};
}

Result<bool> CreateDataProperty(Object* object, const PropertyKey& key, Value value) {
  return object->DefineOwnProperty(key, PropertyDescriptor::data(value, Attr::All));
}

Result<void> CreateDataPropertyOrThrow(Object* object, const PropertyKey& key, Value value) {
  bool success = TRY(CreateDataProperty(object, key, value));
  if (!success) return ThrowTypeError("Cannot define property");
  return {};
}

Result<void> CreateNonEnumerableDataPropertyOrThrow(Object* object, const PropertyKey& key, Value value) {
  return DefinePropertyOrThrow(object, key, PropertyDescriptor::data(value, Attr::WritableConfigurable));
}

Result<void> DefinePropertyOrThrow(Object* object, const PropertyKey& key, const PropertyDescriptor& desc) {
  bool success = TRY(object->DefineOwnProperty(key, desc));
  if (!success) return ThrowTypeError("Cannot redefine property");
  return {};
}

Result<void> DeletePropertyOrThrow(Object* object, const PropertyKey& key) {
  bool success = TRY(object->Delete(key));
  if (!success) return ThrowTypeError("Cannot delete property");
  return {};
}

Result<Value> GetMethod(Value value, const PropertyKey& key) {
  Value func = TRY(GetV(value, key));
  if (func.isNullish()) return Value::undefined();
  if (!IsCallable(func)) return ThrowTypeError("not a function");
  return func;
}

Result<bool> HasProperty(Object* object, const PropertyKey& key) { return object->HasProperty(key); }

Result<bool> HasOwnProperty(Object* object, const PropertyKey& key) {
  auto desc = TRY(object->GetOwnProperty(key));
  return desc.has_value();
}

Result<Value> Call(Value function, Value thisValue, Args args) {
  if (!IsCallable(function)) return ThrowTypeError("not a function");
  return function.asObject()->Call(thisValue, args);
}

Result<Object*> Construct(Object* constructor, Args args, Object* newTarget) {
  if (!newTarget) newTarget = constructor;
  return constructor->Construct(args, newTarget);
}

// 7.3.15 SetIntegrityLevel
Result<bool> SetIntegrityLevel(Object* object, IntegrityLevel level) {
  bool status = TRY(object->PreventExtensions());
  if (!status) return false;
  std::vector<PropertyKey> keys = TRY(object->OwnPropertyKeys());
  if (level == IntegrityLevel::Sealed) {
    for (const PropertyKey& k : keys) {
      PropertyDescriptor desc;
      desc.configurable = false;
      TRY(DefinePropertyOrThrow(object, k, desc));
    }
  } else {
    for (const PropertyKey& k : keys) {
      auto currentDesc = TRY(object->GetOwnProperty(k));
      if (currentDesc) {
        PropertyDescriptor desc;
        desc.configurable = false;
        if (!currentDesc->isAccessorDescriptor()) desc.writable = false;
        TRY(DefinePropertyOrThrow(object, k, desc));
      }
    }
  }
  return true;
}

// 7.3.16 TestIntegrityLevel
Result<bool> TestIntegrityLevel(Object* object, IntegrityLevel level) {
  bool extensible = TRY(IsExtensible(object));
  if (extensible) return false;
  std::vector<PropertyKey> keys = TRY(object->OwnPropertyKeys());
  for (const PropertyKey& k : keys) {
    auto currentDesc = TRY(object->GetOwnProperty(k));
    if (currentDesc) {
      if (*currentDesc->configurable) return false;
      if (level == IntegrityLevel::Frozen && currentDesc->isDataDescriptor() && *currentDesc->writable) return false;
    }
  }
  return true;
}

// 7.3.17 CreateArrayFromList
Object* CreateArrayFromList(Args elements) {
  Realm* realm = vm().currentRealm();
  ArrayObject* array = make<ArrayObject>(realm->intrinsic(Intrinsic::ArrayPrototype), 0);
  uint32_t i = 0;
  for (Value v : elements) array->properties().put(PropertyKey::index(i++), PropertySlot::data(v, Attr::All));
  array->setLengthDirect(i);
  return array;
}

// 7.3.18 LengthOfArrayLike
Result<double> LengthOfArrayLike(Object* object) {
  Value len = TRY(Get(object, PropertyKey(u"length")));
  return ToLength(len);
}

// 7.3.19 CreateListFromArrayLike
Result<ValueVector> CreateListFromArrayLike(Value object, bool onlyPropertyKeys) {
  if (!object.isObject()) return ThrowTypeError("CreateListFromArrayLike called on non-object");
  Object* obj = object.asObject();
  double len = TRY(LengthOfArrayLike(obj));
  if (len > 0xFFFFFFF) return ThrowRangeError("Too many arguments");
  ValueVector list;
  for (double index = 0; index < len; index++) {
    Value next = TRY(Get(obj, PropertyKey::index(uint32_t(index))));
    if (onlyPropertyKeys && !next.isString() && !next.isSymbol())
      return ThrowTypeError("list element is not a property key");
    list.push_back(next);
  }
  return list;
}

// 7.3.20 Invoke
Result<Value> Invoke(Value value, const PropertyKey& key, Args args) {
  Value func = TRY(GetV(value, key));
  return Call(func, value, args);
}

// 7.3.21 OrdinaryHasInstance
Result<bool> OrdinaryHasInstance(Value constructor, Value object) {
  if (!IsCallable(constructor)) return false;
  Object* c = constructor.asObject();
  if (auto* bound = c->as<BoundFunction>()) return InstanceofOperator(object, bound->boundTargetFunction);
  if (!object.isObject()) return false;
  Value p = TRY(Get(c, PropertyKey(u"prototype")));
  if (!p.isObject()) return ThrowTypeError("Function has non-object prototype in instanceof check");
  Object* o = object.asObject();
  while (true) {
    o = TRY(o->GetPrototypeOf());
    if (!o) return false;
    if (o == p.asObject()) return true;
  }
}

// 13.10.2 InstanceofOperator
Result<bool> InstanceofOperator(Value value, Value target) {
  if (!target.isObject()) return ThrowTypeError("Right-hand side of 'instanceof' is not an object");
  Value instOfHandler = TRY(GetMethod(target, PropertyKey(vm().symbols.hasInstance)));
  if (!instOfHandler.isUndefined()) {
    Value result = TRY(Call(instOfHandler, target, {value}));
    return ToBoolean(result);
  }
  if (!IsCallable(target)) return ThrowTypeError("Right-hand side of 'instanceof' is not callable");
  return OrdinaryHasInstance(target, value);
}

// 7.3.22 SpeciesConstructor
Result<Object*> SpeciesConstructor(Object* object, Object* defaultConstructor) {
  Value c = TRY(Get(object, PropertyKey(u"constructor")));
  if (c.isUndefined()) return defaultConstructor;
  if (!c.isObject()) return ThrowTypeError("object.constructor is not an object");
  Value s = TRY(Get(c.asObject(), PropertyKey(vm().symbols.species)));
  if (s.isNullish()) return defaultConstructor;
  if (IsConstructor(s)) return s.asObject();
  return ThrowTypeError("object.constructor[Symbol.species] is not a constructor");
}

// 7.3.23 EnumerableOwnProperties
Result<ValueVector> EnumerableOwnProperties(Object* object, EnumerableOwnKind kind) {
  std::vector<PropertyKey> ownKeys = TRY(object->OwnPropertyKeys());
  ValueVector results;
  for (const PropertyKey& key : ownKeys) {
    if (key.isSymbol()) continue;
    auto desc = TRY(object->GetOwnProperty(key));
    if (!desc || !*desc->enumerable) continue;
    if (kind == EnumerableOwnKind::Key) {
      results.push_back(key.toValue());
      continue;
    }
    Value value = TRY(Get(object, key));
    if (kind == EnumerableOwnKind::Value) {
      results.push_back(value);
    } else {
      results.push_back(CreateArrayFromList({key.toValue(), value}));
    }
  }
  return results;
}

// 7.3.24 GetFunctionRealm
Result<Realm*> GetFunctionRealm(Object* object) {
  if (auto* f = object->as<FunctionObject>()) {
    if (f->realm()) return f->realm();
  }
  if (auto* bound = object->as<BoundFunction>()) return GetFunctionRealm(bound->boundTargetFunction);
  if (auto* proxy = object->as<ProxyObject>()) {
    if (!proxy->proxyHandler) return ThrowTypeError("Cannot get the realm of a revoked proxy");
    return GetFunctionRealm(proxy->proxyTarget);
  }
  return vm().currentRealm();
}

// 7.3.25 CopyDataProperties
Result<void> CopyDataProperties(Object* target, Value source, const std::vector<PropertyKey>& excludedKeys) {
  if (source.isNullish()) return {};
  Object* from = TRY(ToObject(source));
  std::vector<PropertyKey> keys = TRY(from->OwnPropertyKeys());
  for (const PropertyKey& nextKey : keys) {
    bool excluded = false;
    for (const PropertyKey& e : excludedKeys) {
      if (e == nextKey) {
        excluded = true;
        break;
      }
    }
    if (excluded) continue;
    auto desc = TRY(from->GetOwnProperty(nextKey));
    if (desc && *desc->enumerable) {
      Value propValue = TRY(Get(from, nextKey));
      TRY(CreateDataPropertyOrThrow(target, nextKey, propValue));
    }
  }
  return {};
}

// 10.1.14 GetPrototypeFromConstructor
Result<Object*> GetPrototypeFromConstructor(Object* constructor, Intrinsic intrinsicDefaultProto) {
  Value proto = TRY(Get(constructor, PropertyKey(u"prototype")));
  if (!proto.isObject()) {
    Realm* realm = TRY(GetFunctionRealm(constructor));
    return realm->intrinsic(intrinsicDefaultProto);
  }
  return proto.asObject();
}

// 6.2.6.5 ToPropertyDescriptor
Result<PropertyDescriptor> ToPropertyDescriptor(Value object) {
  if (!object.isObject()) return ThrowTypeError("Property description must be an object");
  Object* obj = object.asObject();
  PropertyDescriptor desc;
  if (TRY(HasProperty(obj, PropertyKey(u"enumerable")))) desc.enumerable = ToBoolean(TRY(Get(obj, PropertyKey(u"enumerable"))));
  if (TRY(HasProperty(obj, PropertyKey(u"configurable"))))
    desc.configurable = ToBoolean(TRY(Get(obj, PropertyKey(u"configurable"))));
  if (TRY(HasProperty(obj, PropertyKey(u"value")))) desc.value = TRY(Get(obj, PropertyKey(u"value")));
  if (TRY(HasProperty(obj, PropertyKey(u"writable")))) desc.writable = ToBoolean(TRY(Get(obj, PropertyKey(u"writable"))));
  if (TRY(HasProperty(obj, PropertyKey(u"get")))) {
    Value getter = TRY(Get(obj, PropertyKey(u"get")));
    if (!IsCallable(getter) && !getter.isUndefined()) return ThrowTypeError("Getter must be a function");
    desc.get = getter;
  }
  if (TRY(HasProperty(obj, PropertyKey(u"set")))) {
    Value setter = TRY(Get(obj, PropertyKey(u"set")));
    if (!IsCallable(setter) && !setter.isUndefined()) return ThrowTypeError("Setter must be a function");
    desc.set = setter;
  }
  if ((desc.get || desc.set) && (desc.value || desc.writable))
    return ThrowTypeError("Invalid property descriptor. Cannot both specify accessors and a value or writable attribute");
  return desc;
}

// 6.2.6.4 FromPropertyDescriptor
Value FromPropertyDescriptor(const std::optional<PropertyDescriptor>& desc) {
  if (!desc) return Value::undefined();
  Object* obj = OrdinaryObjectCreate(vm().currentRealm()->intrinsic(Intrinsic::ObjectPrototype));
  auto put = [&](const char16_t* name, Value v) {
    obj->properties().put(PropertyKey(name), PropertySlot::data(v, Attr::All));
  };
  if (desc->value) put(u"value", *desc->value);
  if (desc->writable) put(u"writable", Value::boolean(*desc->writable));
  if (desc->get) put(u"get", *desc->get);
  if (desc->set) put(u"set", *desc->set);
  if (desc->enumerable) put(u"enumerable", Value::boolean(*desc->enumerable));
  if (desc->configurable) put(u"configurable", Value::boolean(*desc->configurable));
  return obj;
}

}  // namespace snowman
