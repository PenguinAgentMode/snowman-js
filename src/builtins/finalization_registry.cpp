// FinalizationRegistry (26.2). Targets are never collected yet, so cleanup callbacks never run.
#include "builtins/util.h"
#include "builtins/weak.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

class FinalizationRegistryObject : public Object {
 public:
  FinalizationRegistryObject(Object* prototype, Value cleanup) : Object(prototype), cleanupCallback(cleanup) {}
  struct Cell {
    Value target;
    Value heldValue;
    Value unregisterToken;  // empty when none
  };
  Value cleanupCallback;
  std::vector<Cell> cells;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(cleanupCallback);
    for (const Cell& c : cells) {
      tracer.visit(c.target);
      tracer.visit(c.heldValue);
      tracer.visit(c.unregisterToken);
    }
  }
};

// 26.2.1.1 FinalizationRegistry ( cleanupCallback )
Result<Value> FinalizationRegistry_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor FinalizationRegistry requires 'new'");
  if (!IsCallable(call.args[0])) return ThrowTypeError("FinalizationRegistry: cleanup must be callable");
  return Value(TRY(OrdinaryCreateFromConstructor<FinalizationRegistryObject>(
      call.newTarget, Intrinsic::FinalizationRegistryPrototype, call.args[0])));
}

Result<FinalizationRegistryObject*> thisRegistry(Value v, const char* method) {
  if (v.isObject()) {
    if (auto* r = v.asObject()->as<FinalizationRegistryObject>()) return r;
  }
  return ThrowTypeError(std::string("FinalizationRegistry.prototype.") + method + " called on incompatible receiver");
}

// 26.2.3.2 FinalizationRegistry.prototype.register ( target, heldValue [ , unregisterToken ] )
Result<Value> FinalizationRegistryPrototype_register(NativeCall& call) {
  FinalizationRegistryObject* r = TRY(thisRegistry(call.thisValue, "register"));
  Value target = call.args[0], heldValue = call.args[1], unregisterToken = call.args[2];
  if (!CanBeHeldWeakly(target)) return ThrowTypeError("FinalizationRegistry.prototype.register: invalid target");
  if (SameValue(target, heldValue)) return ThrowTypeError("FinalizationRegistry.prototype.register: target and holdings must not be same");
  if (!CanBeHeldWeakly(unregisterToken)) {
    if (!unregisterToken.isUndefined()) return ThrowTypeError("FinalizationRegistry.prototype.register: invalid unregister token");
    unregisterToken = Value::empty();
  }
  r->cells.push_back({target, heldValue, unregisterToken});
  return Value::undefined();
}

// 26.2.3.3 FinalizationRegistry.prototype.unregister ( unregisterToken )
Result<Value> FinalizationRegistryPrototype_unregister(NativeCall& call) {
  FinalizationRegistryObject* r = TRY(thisRegistry(call.thisValue, "unregister"));
  Value token = call.args[0];
  if (!CanBeHeldWeakly(token)) return ThrowTypeError("FinalizationRegistry.prototype.unregister: invalid unregister token");
  bool removed = false;
  std::vector<FinalizationRegistryObject::Cell> kept;
  for (const auto& c : r->cells) {
    if (!c.unregisterToken.isEmpty() && SameValue(c.unregisterToken, token)) removed = true;
    else kept.push_back(c);
  }
  r->cells = std::move(kept);
  return boolean(removed);
}

void installFinalizationRegistry(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  defineConstructor(realm, Intrinsic::FinalizationRegistry, FinalizationRegistry_construct, 1,
                    PropertyKey(u"FinalizationRegistry"), proto, Intrinsic::FinalizationRegistryPrototype);
  defineFunction(realm, proto, PropertyKey(u"register"), 2, FinalizationRegistryPrototype_register);
  defineFunction(realm, proto, PropertyKey(u"unregister"), 1, FinalizationRegistryPrototype_unregister);
  defineValue(proto, PropertyKey(symbols().toStringTag), str(u"FinalizationRegistry"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(FinalizationRegistry, kOrderDefault, installFinalizationRegistry);

}  // namespace snowman::builtins
