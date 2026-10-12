// WeakSet (24.4).
#include "builtins/util.h"
#include "builtins/weak.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

// 24.4.1.1 WeakSet ( [ iterable ] )
Result<Value> WeakSet_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor WeakSet requires 'new'");
  WeakSetObject* set = TRY(OrdinaryCreateFromConstructor<WeakSetObject>(call.newTarget, Intrinsic::WeakSetPrototype));
  Value iterable = call.args[0];
  if (iterable.isNullish()) return Value(set);
  Value adder = TRY(Get(set, PropertyKey(u"add")));
  if (!IsCallable(adder)) return ThrowTypeError("WeakSet.prototype.add is not a function");
  IteratorRecord record = TRY(GetIterator(iterable, IteratorKind::Sync));
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(record));
    if (!next) return Value(set);
    TRY_CLOSE(record, Call(adder, set, {*next}));
  }
}

Result<WeakSetObject*> thisWeakSet(Value v, const char* method) {
  if (v.isObject()) {
    if (auto* s = v.asObject()->as<WeakSetObject>()) return s;
  }
  return ThrowTypeError(std::string("WeakSet.prototype.") + method + " called on incompatible receiver");
}

Result<Value> WeakSetPrototype_add(NativeCall& call) {
  WeakSetObject* s = TRY(thisWeakSet(call.thisValue, "add"));
  if (!CanBeHeldWeakly(call.args[0])) return ThrowTypeError("Invalid value used in weak set");
  s->entries[call.args[0].asCell()] = Value::boolean(true);
  return call.thisValue;
}

Result<Value> WeakSetPrototype_delete(NativeCall& call) {
  WeakSetObject* s = TRY(thisWeakSet(call.thisValue, "delete"));
  if (!CanBeHeldWeakly(call.args[0])) return boolean(false);
  return boolean(s->entries.erase(call.args[0].asCell()) > 0);
}

Result<Value> WeakSetPrototype_has(NativeCall& call) {
  WeakSetObject* s = TRY(thisWeakSet(call.thisValue, "has"));
  if (!CanBeHeldWeakly(call.args[0])) return boolean(false);
  return boolean(s->entries.count(call.args[0].asCell()) > 0);
}

void installWeakSet(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  defineConstructor(realm, Intrinsic::WeakSet, WeakSet_construct, 0, PropertyKey(u"WeakSet"), proto, Intrinsic::WeakSetPrototype);
  defineFunction(realm, proto, PropertyKey(u"add"), 1, WeakSetPrototype_add);
  defineFunction(realm, proto, PropertyKey(u"delete"), 1, WeakSetPrototype_delete);
  defineFunction(realm, proto, PropertyKey(u"has"), 1, WeakSetPrototype_has);
  defineValue(proto, PropertyKey(symbols().toStringTag), str(u"WeakSet"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(WeakSet, kOrderDefault, installWeakSet);

}  // namespace snowman::builtins
