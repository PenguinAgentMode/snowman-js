// WeakMap (24.3).
#include "builtins/util.h"
#include "builtins/weak.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

// 24.3.1.1 WeakMap ( [ iterable ] )
Result<Value> WeakMap_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor WeakMap requires 'new'");
  WeakMapObject* map = TRY(OrdinaryCreateFromConstructor<WeakMapObject>(call.newTarget, Intrinsic::WeakMapPrototype));
  Value iterable = call.args[0];
  if (iterable.isNullish()) return Value(map);
  Value adder = TRY(Get(map, PropertyKey(u"set")));
  if (!IsCallable(adder)) return ThrowTypeError("WeakMap.prototype.set is not a function");
  IteratorRecord record = TRY(GetIterator(iterable, IteratorKind::Sync));
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(record));
    if (!next) return Value(map);
    if (!next->isObject()) return closeIteratorWithThrow(record, ThrowTypeError("Iterator value is not an entry object").value);
    Value k = TRY_CLOSE(record, Get(next->asObject(), PropertyKey::index(0)));
    Value v = TRY_CLOSE(record, Get(next->asObject(), PropertyKey::index(1)));
    TRY_CLOSE(record, Call(adder, map, {k, v}));
  }
}

Result<WeakMapObject*> thisWeakMap(Value v, const char* method) {
  if (v.isObject()) {
    if (auto* m = v.asObject()->as<WeakMapObject>()) return m;
  }
  return ThrowTypeError(std::string("WeakMap.prototype.") + method + " called on incompatible receiver");
}

Result<Value> WeakMapPrototype_delete(NativeCall& call) {
  WeakMapObject* m = TRY(thisWeakMap(call.thisValue, "delete"));
  if (!CanBeHeldWeakly(call.args[0])) return boolean(false);
  return boolean(m->entries.erase(call.args[0].asCell()) > 0);
}

Result<Value> WeakMapPrototype_get(NativeCall& call) {
  WeakMapObject* m = TRY(thisWeakMap(call.thisValue, "get"));
  if (!CanBeHeldWeakly(call.args[0])) return Value::undefined();
  auto it = m->entries.find(call.args[0].asCell());
  return it == m->entries.end() ? Value::undefined() : it->second;
}

Result<Value> WeakMapPrototype_has(NativeCall& call) {
  WeakMapObject* m = TRY(thisWeakMap(call.thisValue, "has"));
  if (!CanBeHeldWeakly(call.args[0])) return boolean(false);
  return boolean(m->entries.count(call.args[0].asCell()) > 0);
}

Result<Value> WeakMapPrototype_set(NativeCall& call) {
  WeakMapObject* m = TRY(thisWeakMap(call.thisValue, "set"));
  if (!CanBeHeldWeakly(call.args[0])) return ThrowTypeError("Invalid value used as weak map key");
  m->entries[call.args[0].asCell()] = call.args[1];
  return call.thisValue;
}

void installWeakMap(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  defineConstructor(realm, Intrinsic::WeakMap, WeakMap_construct, 0, PropertyKey(u"WeakMap"), proto, Intrinsic::WeakMapPrototype);
  defineFunction(realm, proto, PropertyKey(u"delete"), 1, WeakMapPrototype_delete);
  defineFunction(realm, proto, PropertyKey(u"get"), 1, WeakMapPrototype_get);
  defineFunction(realm, proto, PropertyKey(u"has"), 1, WeakMapPrototype_has);
  defineFunction(realm, proto, PropertyKey(u"set"), 2, WeakMapPrototype_set);
  defineValue(proto, PropertyKey(symbols().toStringTag), str(u"WeakMap"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(WeakMap, kOrderDefault, installWeakMap);

}  // namespace snowman::builtins
