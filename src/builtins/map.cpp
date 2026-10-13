// Map (24.1) and %MapIteratorPrototype%.
#include "builtins/keyed_collection.h"
#include "builtins/util.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

// 24.1.1.2 AddEntriesFromIterable ( target, iterable, adder )
Result<Value> AddEntriesFromIterable(Object* target, Value iterable, Value adder) {
  IteratorRecord record = TRY(GetIterator(iterable, IteratorKind::Sync));
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(record));
    if (!next) return Value(target);
    if (!next->isObject()) return closeIteratorWithThrow(record, ThrowTypeError("Iterator value is not an entry object").value);
    Value k = TRY_CLOSE(record, Get(next->asObject(), PropertyKey::index(0)));
    Value v = TRY_CLOSE(record, Get(next->asObject(), PropertyKey::index(1)));
    TRY_CLOSE(record, Call(adder, target, {k, v}));
  }
}

// 24.1.1.1 Map ( [ iterable ] )
Result<Value> Map_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Map requires 'new'");
  MapObject* map = TRY(OrdinaryCreateFromConstructor<MapObject>(call.newTarget, Intrinsic::MapPrototype));
  Value iterable = call.args[0];
  if (iterable.isNullish()) return Value(map);
  Value adder = TRY(Get(map, PropertyKey(u"set")));
  if (!IsCallable(adder)) return ThrowTypeError("Map.prototype.set is not a function");
  return AddEntriesFromIterable(map, iterable, adder);
}

// 24.1.2.1 Map.groupBy ( items, callback )
Result<Value> Map_groupBy(NativeCall& call) {
  Value items = call.args[0], callback = call.args[1];
  TRY(RequireObjectCoercible(items));
  if (!IsCallable(callback)) return ThrowTypeError("Map.groupBy: callback is not a function");
  MapObject* map = make<MapObject>(intrinsic(Intrinsic::MapPrototype));
  IteratorRecord record = TRY(GetIterator(items, IteratorKind::Sync));
  std::vector<ValueVector> groups;
  OrderedTable groupIndex;
  ValueVector groupKeys;  // groupIndex is not traced
  double k = 0;
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(record));
    if (!next) break;
    Value key = TRY_CLOSE(record, Call(callback, Value::undefined(), {*next, num(k)}));
    key = CanonicalizeKeyedCollectionKey(key);
    long existing = groupIndex.find(key);
    if (existing < 0) {
      groupIndex.set(key, num(double(groups.size())));
      groupKeys.push_back(key);
      groups.emplace_back();
      groups.back().push_back(*next);
    } else {
      groups[size_t(groupIndex.entries()[size_t(existing)].value.asNumber())].push_back(*next);
    }
    k++;
  }
  for (const auto& e : groupIndex.entries()) map->mapData.set(e.key, CreateArrayFromList(groups[size_t(e.value.asNumber())]));
  return Value(map);
}

Result<Value> Map_species(NativeCall& call) { return call.thisValue; }

Result<MapObject*> thisMap(Value v, const char* method) {
  if (v.isObject()) {
    if (auto* m = v.asObject()->as<MapObject>()) return m;
  }
  return ThrowTypeError(std::string("Map.prototype.") + method + " called on incompatible receiver");
}

Result<Value> MapPrototype_clear(NativeCall& call) {
  MapObject* m = TRY(thisMap(call.thisValue, "clear"));
  m->mapData.clear();
  return Value::undefined();
}

Result<Value> MapPrototype_delete(NativeCall& call) {
  MapObject* m = TRY(thisMap(call.thisValue, "delete"));
  return boolean(m->mapData.remove(CanonicalizeKeyedCollectionKey(call.args[0])));
}

Result<Value> MapPrototype_get(NativeCall& call) {
  MapObject* m = TRY(thisMap(call.thisValue, "get"));
  return m->mapData.get(CanonicalizeKeyedCollectionKey(call.args[0]));
}

Result<Value> MapPrototype_has(NativeCall& call) {
  MapObject* m = TRY(thisMap(call.thisValue, "has"));
  return boolean(m->mapData.has(CanonicalizeKeyedCollectionKey(call.args[0])));
}

Result<Value> MapPrototype_set(NativeCall& call) {
  MapObject* m = TRY(thisMap(call.thisValue, "set"));
  m->mapData.set(CanonicalizeKeyedCollectionKey(call.args[0]), call.args[1]);
  return call.thisValue;
}

// Map.prototype.getOrInsert ( key, value )
Result<Value> MapPrototype_getOrInsert(NativeCall& call) {
  MapObject* m = TRY(thisMap(call.thisValue, "getOrInsert"));
  Value key = CanonicalizeKeyedCollectionKey(call.args[0]);
  long i = m->mapData.find(key);
  if (i >= 0) return m->mapData.entries()[size_t(i)].value;
  m->mapData.set(key, call.args[1]);
  return call.args[1];
}

// Map.prototype.getOrInsertComputed ( key, callbackfn )
Result<Value> MapPrototype_getOrInsertComputed(NativeCall& call) {
  MapObject* m = TRY(thisMap(call.thisValue, "getOrInsertComputed"));
  if (!IsCallable(call.args[1])) return ThrowTypeError("Map.prototype.getOrInsertComputed: callback is not a function");
  Value key = CanonicalizeKeyedCollectionKey(call.args[0]);
  long i = m->mapData.find(key);
  if (i >= 0) return m->mapData.entries()[size_t(i)].value;
  Value value = TRY(Call(call.args[1], Value::undefined(), {key}));
  // The callback may have inserted the key itself: overwrite in place.
  m->mapData.set(key, value);
  return value;
}

Result<Value> MapPrototype_size(NativeCall& call) {
  MapObject* m = TRY(thisMap(call.thisValue, "size"));
  return num(double(m->mapData.liveCount()));
}

// 24.1.3.5 Map.prototype.forEach ( callback [ , thisArg ] )
Result<Value> MapPrototype_forEach(NativeCall& call) {
  MapObject* m = TRY(thisMap(call.thisValue, "forEach"));
  Value callback = call.args[0];
  if (!IsCallable(callback)) return ThrowTypeError("Map.prototype.forEach: callback is not a function");
  OrderedTable::Position pos = m->mapData.begin();
  while (true) {
    size_t i = m->mapData.adjust(pos);
    if (i >= m->mapData.entries().size()) break;
    OrderedTable::Entry e = m->mapData.entries()[i];
    pos.index++;
    if (e.key.isEmpty()) continue;
    TRY(Call(callback, call.args[1], {e.value, e.key, Value(m)}));
  }
  return Value::undefined();
}

enum class IterKind { Key, Value, KeyValue };

class MapIterator : public Object {
 public:
  MapIterator(Object* prototype, MapObject* map, IterKind kind)
      : Object(prototype), map(map), kind(kind), position(map->mapData.begin()) {}
  MapObject* map;  // nullptr once exhausted
  IterKind kind;
  OrderedTable::Position position;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(reinterpret_cast<Cell*>(map));
  }
};

Result<Value> createIterator(NativeCall& call, IterKind kind, const char* method) {
  MapObject* m = TRY(thisMap(call.thisValue, method));
  return Value(make<MapIterator>(intrinsic(Intrinsic::MapIteratorPrototype), m, kind));
}

Result<Value> MapPrototype_entries(NativeCall& call) { return createIterator(call, IterKind::KeyValue, "entries"); }
Result<Value> MapPrototype_keys(NativeCall& call) { return createIterator(call, IterKind::Key, "keys"); }
Result<Value> MapPrototype_values(NativeCall& call) { return createIterator(call, IterKind::Value, "values"); }

// 24.1.5.2.1 %MapIteratorPrototype%.next ( )
Result<Value> MapIteratorPrototype_next(NativeCall& call) {
  MapIterator* it = call.thisValue.isObject() ? call.thisValue.asObject()->as<MapIterator>() : nullptr;
  if (!it) return ThrowTypeError("next method called on incompatible receiver");
  while (it->map) {
    OrderedTable& table = it->map->mapData;
    size_t i = table.adjust(it->position);
    if (i >= table.entries().size()) {
      it->map = nullptr;
      break;
    }
    OrderedTable::Entry e = table.entries()[i];
    it->position.index++;
    if (e.key.isEmpty()) continue;
    Value result = it->kind == IterKind::Key     ? e.key
                   : it->kind == IterKind::Value ? e.value
                                                 : Value(CreateArrayFromList({e.key, e.value}));
    return Value(CreateIteratorResultObject(result, false));
  }
  return Value(CreateIteratorResultObject(Value::undefined(), true));
}

void installMap(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor = defineConstructor(realm, Intrinsic::Map, Map_construct, 0, PropertyKey(u"Map"), proto, Intrinsic::MapPrototype);
  defineFunction(realm, ctor, PropertyKey(u"groupBy"), 2, Map_groupBy);
  defineAccessor(realm, ctor, PropertyKey(symbols().species), Map_species, nullptr);

  defineFunction(realm, proto, PropertyKey(u"clear"), 0, MapPrototype_clear);
  defineFunction(realm, proto, PropertyKey(u"delete"), 1, MapPrototype_delete);
  BuiltinFunction* entries = defineFunction(realm, proto, PropertyKey(u"entries"), 0, MapPrototype_entries);
  defineFunction(realm, proto, PropertyKey(u"forEach"), 1, MapPrototype_forEach);
  defineFunction(realm, proto, PropertyKey(u"get"), 1, MapPrototype_get);
  defineFunction(realm, proto, PropertyKey(u"getOrInsert"), 2, MapPrototype_getOrInsert);
  defineFunction(realm, proto, PropertyKey(u"getOrInsertComputed"), 2, MapPrototype_getOrInsertComputed);
  defineFunction(realm, proto, PropertyKey(u"has"), 1, MapPrototype_has);
  defineFunction(realm, proto, PropertyKey(u"keys"), 0, MapPrototype_keys);
  defineFunction(realm, proto, PropertyKey(u"set"), 2, MapPrototype_set);
  defineAccessor(realm, proto, PropertyKey(u"size"), MapPrototype_size, nullptr);
  defineFunction(realm, proto, PropertyKey(u"values"), 0, MapPrototype_values);
  defineValue(proto, PropertyKey(symbols().iterator), entries);
  defineValue(proto, PropertyKey(symbols().toStringTag), str(u"Map"), Attr::Configurable);

  Object* iterProto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::IteratorPrototype));
  realm.setIntrinsic(Intrinsic::MapIteratorPrototype, iterProto);
  defineFunction(realm, iterProto, PropertyKey(u"next"), 0, MapIteratorPrototype_next);
  defineValue(iterProto, PropertyKey(symbols().toStringTag), str(u"Map Iterator"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(Map, kOrderDefault, installMap);

}  // namespace snowman::builtins
