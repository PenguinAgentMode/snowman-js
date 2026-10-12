// WeakRef (26.1).
#include "builtins/util.h"
#include "builtins/weak.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

class WeakRefObject : public Object {
 public:
  WeakRefObject(Object* prototype, Value target) : Object(prototype), target(target) {}
  Value target;  // [[WeakRefTarget]]
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(target);
  }
};

// 26.1.1.1 WeakRef ( target )
Result<Value> WeakRef_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor WeakRef requires 'new'");
  if (!CanBeHeldWeakly(call.args[0])) return ThrowTypeError("WeakRef: invalid target");
  return Value(TRY(OrdinaryCreateFromConstructor<WeakRefObject>(call.newTarget, Intrinsic::WeakRefPrototype, call.args[0])));
}

// 26.1.3.2 WeakRef.prototype.deref ( )
Result<Value> WeakRefPrototype_deref(NativeCall& call) {
  WeakRefObject* w = call.thisValue.isObject() ? call.thisValue.asObject()->as<WeakRefObject>() : nullptr;
  if (!w) return ThrowTypeError("WeakRef.prototype.deref called on incompatible receiver");
  return w->target;
}

void installWeakRef(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  defineConstructor(realm, Intrinsic::WeakRef, WeakRef_construct, 1, PropertyKey(u"WeakRef"), proto, Intrinsic::WeakRefPrototype);
  defineFunction(realm, proto, PropertyKey(u"deref"), 0, WeakRefPrototype_deref);
  defineValue(proto, PropertyKey(symbols().toStringTag), str(u"WeakRef"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(WeakRef, kOrderDefault, installWeakRef);

}  // namespace snowman::builtins
