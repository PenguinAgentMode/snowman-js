// Set (24.2), including the ES2025 set methods, and %SetIteratorPrototype%.
#include <cmath>

#include "builtins/keyed_collection.h"
#include "builtins/util.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

// 24.2.2.1 Set ( [ iterable ] )
Result<Value> Set_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Set requires 'new'");
  SetObject* set = TRY(OrdinaryCreateFromConstructor<SetObject>(call.newTarget, Intrinsic::SetPrototype));
  Value iterable = call.args[0];
  if (iterable.isNullish()) return Value(set);
  Value adder = TRY(Get(set, PropertyKey(u"add")));
  if (!IsCallable(adder)) return ThrowTypeError("Set.prototype.add is not a function");
  IteratorRecord record = TRY(GetIterator(iterable, IteratorKind::Sync));
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(record));
    if (!next) return Value(set);
    TRY_CLOSE(record, Call(adder, set, {*next}));
  }
}

Result<Value> Set_species(NativeCall& call) { return call.thisValue; }

Result<SetObject*> thisSet(Value v, const char* method) {
  if (v.isObject()) {
    if (auto* s = v.asObject()->as<SetObject>()) return s;
  }
  return ThrowTypeError(std::string("Set.prototype.") + method + " called on incompatible receiver");
}

Result<Value> SetPrototype_add(NativeCall& call) {
  SetObject* s = TRY(thisSet(call.thisValue, "add"));
  Value value = CanonicalizeKeyedCollectionKey(call.args[0]);
  if (!s->setData.has(value)) s->setData.set(value, value);
  return call.thisValue;
}

Result<Value> SetPrototype_clear(NativeCall& call) {
  SetObject* s = TRY(thisSet(call.thisValue, "clear"));
  s->setData.clear();
  return Value::undefined();
}

Result<Value> SetPrototype_delete(NativeCall& call) {
  SetObject* s = TRY(thisSet(call.thisValue, "delete"));
  return boolean(s->setData.remove(CanonicalizeKeyedCollectionKey(call.args[0])));
}

Result<Value> SetPrototype_has(NativeCall& call) {
  SetObject* s = TRY(thisSet(call.thisValue, "has"));
  return boolean(s->setData.has(CanonicalizeKeyedCollectionKey(call.args[0])));
}

Result<Value> SetPrototype_size(NativeCall& call) {
  SetObject* s = TRY(thisSet(call.thisValue, "size"));
  return num(double(s->setData.liveCount()));
}

// 24.2.4.8 Set.prototype.forEach ( callback [ , thisArg ] )
Result<Value> SetPrototype_forEach(NativeCall& call) {
  SetObject* s = TRY(thisSet(call.thisValue, "forEach"));
  Value callback = call.args[0];
  if (!IsCallable(callback)) return ThrowTypeError("Set.prototype.forEach: callback is not a function");
  OrderedTable::Position pos = s->setData.begin();
  while (true) {
    size_t i = s->setData.adjust(pos);
    if (i >= s->setData.entries().size()) break;
    Value e = s->setData.entries()[i].key;
    pos.index++;
    if (e.isEmpty()) continue;
    TRY(Call(callback, call.args[1], {e, e, Value(s)}));
  }
  return Value::undefined();
}

// 24.2.1.2 Set Records and GetSetRecord ( obj )
struct SetRecord {
  Object* setObject;
  double size;
  Value has;
  Value keys;
};

Result<SetRecord> GetSetRecord(Value obj) {
  if (!obj.isObject()) return ThrowTypeError("set method argument is not an object");
  Object* o = obj.asObject();
  Value rawSize = TRY(Get(o, PropertyKey(u"size")));
  double numSize = TRY(ToNumber(rawSize));
  if (std::isnan(numSize)) return ThrowTypeError("set-like object has an invalid size");
  double intSize = TRY(ToIntegerOrInfinity(num(numSize)));
  if (intSize < 0) return ThrowRangeError("set-like object has a negative size");
  Value has = TRY(Get(o, PropertyKey(u"has")));
  if (!IsCallable(has)) return ThrowTypeError("set-like object's has is not a function");
  Value keys = TRY(Get(o, PropertyKey(u"keys")));
  if (!IsCallable(keys)) return ThrowTypeError("set-like object's keys is not a function");
  return SetRecord{o, intSize, has, keys};
}

Result<IteratorRecord> keysIterator(const SetRecord& rec) {
  Value iterator = TRY(Call(rec.keys, rec.setObject));
  if (!iterator.isObject()) return ThrowTypeError("keys() did not return an object");
  Value nextMethod = TRY(Get(iterator.asObject(), PropertyKey(u"next")));
  return IteratorRecord{iterator.asObject(), nextMethod, false};
}

SetObject* newSet() { return make<SetObject>(intrinsic(Intrinsic::SetPrototype)); }

void copyInto(OrderedTable& to, const OrderedTable& from) {
  for (const auto& e : from.entries()) {
    if (!e.key.isEmpty()) to.set(e.key, e.key);
  }
}

// Visits this set's elements by index while `body` may call out to JS (and mutate the set).
template <class F>
Result<bool> forEachElement(SetObject* o, F&& body) {
  OrderedTable::Position pos = o->setData.begin();
  while (true) {
    size_t i = o->setData.adjust(pos);
    if (i >= o->setData.entries().size()) return true;
    Value e = o->setData.entries()[i].key;
    pos.index++;
    if (e.isEmpty()) continue;
    bool keepGoing = TRY(body(e));
    if (!keepGoing) return false;
  }
}

// 24.2.4.18 Set.prototype.union ( other )
Result<Value> SetPrototype_union(NativeCall& call) {
  SetObject* o = TRY(thisSet(call.thisValue, "union"));
  SetRecord otherRec = TRY(GetSetRecord(call.args[0]));
  IteratorRecord keysIter = TRY(keysIterator(otherRec));
  SetObject* result = newSet();
  copyInto(result->setData, o->setData);
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(keysIter));
    if (!next) break;
    Value v = CanonicalizeKeyedCollectionKey(*next);
    if (!result->setData.has(v)) result->setData.set(v, v);
  }
  return Value(result);
}

// 24.2.4.9 Set.prototype.intersection ( other )
Result<Value> SetPrototype_intersection(NativeCall& call) {
  SetObject* o = TRY(thisSet(call.thisValue, "intersection"));
  SetRecord otherRec = TRY(GetSetRecord(call.args[0]));
  SetObject* result = newSet();
  if (double(o->setData.liveCount()) <= otherRec.size) {
    TRY(forEachElement(o, [&](Value e) -> Result<bool> {
      Value inOther = TRY(Call(otherRec.has, otherRec.setObject, {e}));
      if (ToBoolean(inOther) && !result->setData.has(e)) result->setData.set(e, e);
      return true;
    }));
  } else {
    IteratorRecord keysIter = TRY(keysIterator(otherRec));
    while (true) {
      std::optional<Value> next = TRY(IteratorStepValue(keysIter));
      if (!next) break;
      Value v = CanonicalizeKeyedCollectionKey(*next);
      if (o->setData.has(v) && !result->setData.has(v)) result->setData.set(v, v);
    }
  }
  return Value(result);
}

// 24.2.4.5 Set.prototype.difference ( other )
Result<Value> SetPrototype_difference(NativeCall& call) {
  SetObject* o = TRY(thisSet(call.thisValue, "difference"));
  SetRecord otherRec = TRY(GetSetRecord(call.args[0]));
  SetObject* result = newSet();
  copyInto(result->setData, o->setData);
  if (double(o->setData.liveCount()) <= otherRec.size) {
    std::vector<Value> elements;
    for (const auto& e : result->setData.entries()) elements.push_back(e.key);
    ValueVector rooted;
    for (Value e : elements) rooted.push_back(e);
    for (Value e : rooted) {
      Value inOther = TRY(Call(otherRec.has, otherRec.setObject, {e}));
      if (ToBoolean(inOther)) result->setData.remove(e);
    }
  } else {
    IteratorRecord keysIter = TRY(keysIterator(otherRec));
    while (true) {
      std::optional<Value> next = TRY(IteratorStepValue(keysIter));
      if (!next) break;
      result->setData.remove(CanonicalizeKeyedCollectionKey(*next));
    }
  }
  return Value(result);
}

// 24.2.4.15 Set.prototype.symmetricDifference ( other )
Result<Value> SetPrototype_symmetricDifference(NativeCall& call) {
  SetObject* o = TRY(thisSet(call.thisValue, "symmetricDifference"));
  SetRecord otherRec = TRY(GetSetRecord(call.args[0]));
  IteratorRecord keysIter = TRY(keysIterator(otherRec));
  SetObject* result = newSet();
  copyInto(result->setData, o->setData);
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(keysIter));
    if (!next) break;
    Value v = CanonicalizeKeyedCollectionKey(*next);
    bool alreadyInResult = result->setData.has(v);
    if (o->setData.has(v)) {
      if (alreadyInResult) result->setData.remove(v);
    } else if (!alreadyInResult) {
      result->setData.set(v, v);
    }
  }
  return Value(result);
}

// 24.2.4.11 Set.prototype.isSubsetOf ( other )
Result<Value> SetPrototype_isSubsetOf(NativeCall& call) {
  SetObject* o = TRY(thisSet(call.thisValue, "isSubsetOf"));
  SetRecord otherRec = TRY(GetSetRecord(call.args[0]));
  if (double(o->setData.liveCount()) > otherRec.size) return boolean(false);
  bool all = TRY(forEachElement(o, [&](Value e) -> Result<bool> {
    Value inOther = TRY(Call(otherRec.has, otherRec.setObject, {e}));
    return ToBoolean(inOther);
  }));
  return boolean(all);
}

// 24.2.4.12 Set.prototype.isSupersetOf ( other )
Result<Value> SetPrototype_isSupersetOf(NativeCall& call) {
  SetObject* o = TRY(thisSet(call.thisValue, "isSupersetOf"));
  SetRecord otherRec = TRY(GetSetRecord(call.args[0]));
  if (double(o->setData.liveCount()) < otherRec.size) return boolean(false);
  IteratorRecord keysIter = TRY(keysIterator(otherRec));
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(keysIter));
    if (!next) return boolean(true);
    if (!o->setData.has(CanonicalizeKeyedCollectionKey(*next))) {
      TRY(IteratorClose(keysIter, Result<void>()));
      return boolean(false);
    }
  }
}

// 24.2.4.10 Set.prototype.isDisjointFrom ( other )
Result<Value> SetPrototype_isDisjointFrom(NativeCall& call) {
  SetObject* o = TRY(thisSet(call.thisValue, "isDisjointFrom"));
  SetRecord otherRec = TRY(GetSetRecord(call.args[0]));
  if (double(o->setData.liveCount()) <= otherRec.size) {
    bool disjoint = TRY(forEachElement(o, [&](Value e) -> Result<bool> {
      Value inOther = TRY(Call(otherRec.has, otherRec.setObject, {e}));
      return !ToBoolean(inOther);
    }));
    return boolean(disjoint);
  }
  IteratorRecord keysIter = TRY(keysIterator(otherRec));
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(keysIter));
    if (!next) return boolean(true);
    if (o->setData.has(CanonicalizeKeyedCollectionKey(*next))) {
      TRY(IteratorClose(keysIter, Result<void>()));
      return boolean(false);
    }
  }
}

enum class IterKind { Value, KeyValue };

class SetIterator : public Object {
 public:
  SetIterator(Object* prototype, SetObject* set, IterKind kind)
      : Object(prototype), set(set), kind(kind), position(set->setData.begin()) {}
  SetObject* set;  // nullptr once exhausted
  IterKind kind;
  OrderedTable::Position position;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(reinterpret_cast<Cell*>(set));
  }
};

Result<Value> SetPrototype_entries(NativeCall& call) {
  SetObject* s = TRY(thisSet(call.thisValue, "entries"));
  return Value(make<SetIterator>(intrinsic(Intrinsic::SetIteratorPrototype), s, IterKind::KeyValue));
}

Result<Value> SetPrototype_values(NativeCall& call) {
  SetObject* s = TRY(thisSet(call.thisValue, "values"));
  return Value(make<SetIterator>(intrinsic(Intrinsic::SetIteratorPrototype), s, IterKind::Value));
}

// 24.2.6.2.1 %SetIteratorPrototype%.next ( )
Result<Value> SetIteratorPrototype_next(NativeCall& call) {
  SetIterator* it = call.thisValue.isObject() ? call.thisValue.asObject()->as<SetIterator>() : nullptr;
  if (!it) return ThrowTypeError("next method called on incompatible receiver");
  while (it->set) {
    OrderedTable& table = it->set->setData;
    size_t i = table.adjust(it->position);
    if (i >= table.entries().size()) {
      it->set = nullptr;
      break;
    }
    Value e = table.entries()[i].key;
    it->position.index++;
    if (e.isEmpty()) continue;
    Value result = it->kind == IterKind::Value ? e : Value(CreateArrayFromList({e, e}));
    return Value(CreateIteratorResultObject(result, false));
  }
  return Value(CreateIteratorResultObject(Value::undefined(), true));
}

void installSet(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor = defineConstructor(realm, Intrinsic::Set, Set_construct, 0, PropertyKey(u"Set"), proto, Intrinsic::SetPrototype);
  defineAccessor(realm, ctor, PropertyKey(symbols().species), Set_species, nullptr);

  defineFunction(realm, proto, PropertyKey(u"add"), 1, SetPrototype_add);
  defineFunction(realm, proto, PropertyKey(u"clear"), 0, SetPrototype_clear);
  defineFunction(realm, proto, PropertyKey(u"delete"), 1, SetPrototype_delete);
  defineFunction(realm, proto, PropertyKey(u"difference"), 1, SetPrototype_difference);
  defineFunction(realm, proto, PropertyKey(u"entries"), 0, SetPrototype_entries);
  defineFunction(realm, proto, PropertyKey(u"forEach"), 1, SetPrototype_forEach);
  defineFunction(realm, proto, PropertyKey(u"has"), 1, SetPrototype_has);
  defineFunction(realm, proto, PropertyKey(u"intersection"), 1, SetPrototype_intersection);
  defineFunction(realm, proto, PropertyKey(u"isDisjointFrom"), 1, SetPrototype_isDisjointFrom);
  defineFunction(realm, proto, PropertyKey(u"isSubsetOf"), 1, SetPrototype_isSubsetOf);
  defineFunction(realm, proto, PropertyKey(u"isSupersetOf"), 1, SetPrototype_isSupersetOf);
  defineAccessor(realm, proto, PropertyKey(u"size"), SetPrototype_size, nullptr);
  defineFunction(realm, proto, PropertyKey(u"symmetricDifference"), 1, SetPrototype_symmetricDifference);
  defineFunction(realm, proto, PropertyKey(u"union"), 1, SetPrototype_union);
  BuiltinFunction* values = defineFunction(realm, proto, PropertyKey(u"values"), 0, SetPrototype_values);
  defineValue(proto, PropertyKey(u"keys"), values);
  defineValue(proto, PropertyKey(symbols().iterator), values);
  defineValue(proto, PropertyKey(symbols().toStringTag), str(u"Set"), Attr::Configurable);

  Object* iterProto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::IteratorPrototype));
  realm.setIntrinsic(Intrinsic::SetIteratorPrototype, iterProto);
  defineFunction(realm, iterProto, PropertyKey(u"next"), 0, SetIteratorPrototype_next);
  defineValue(iterProto, PropertyKey(symbols().toStringTag), str(u"Set Iterator"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(Set, kOrderDefault, installSet);

}  // namespace snowman::builtins
