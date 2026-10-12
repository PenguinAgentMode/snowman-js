// DisposableStack and Iterator.prototype [ @@dispose ] (explicit resource management).
#include "builtins/disposable.h"
#include "builtins/util.h"
#include "runtime/function.h"

namespace snowman::builtins {

// GetDisposeMethod for sync-dispose, and CreateDisposableResource.
Result<void> AddDisposableResource(DisposeCapability* capability, Value v, DisposeHint hint, Value method) {
  DisposableResource resource{v, hint, method};
  if (method.isEmpty()) {
    if (v.isNullish()) {
      if (hint == DisposeHint::Sync) return {};
      resource = {Value::undefined(), hint, Value::undefined()};
    } else {
      if (!v.isObject()) return ThrowTypeError("disposable resource is not an object");
      Value m;
      if (hint == DisposeHint::Async) {
        m = TRY(GetMethod(v, PropertyKey(symbols().asyncDispose)));
        if (m.isUndefined()) m = TRY(GetMethod(v, PropertyKey(symbols().dispose)));
      } else {
        m = TRY(GetMethod(v, PropertyKey(symbols().dispose)));
      }
      if (m.isUndefined()) return ThrowTypeError("resource has no dispose method");
      resource.disposeMethod = m;
    }
  } else if (!IsCallable(method)) {
    return ThrowTypeError("dispose method is not callable");
  }
  capability->stack.push_back(resource);
  return {};
}

Object* NewSuppressedError(Value error, Value suppressed) {
  Object* e = make<ErrorObject>(intrinsic(Intrinsic::SuppressedErrorPrototype));
  e->properties().put(PropertyKey(u"error"), PropertySlot::data(error, Attr::WritableConfigurable));
  e->properties().put(PropertyKey(u"suppressed"), PropertySlot::data(suppressed, Attr::WritableConfigurable));
  return e;
}

namespace {

// DisposeResources ( disposeCapability, completion ) for sync-dispose resources only.
Result<void> DisposeResourcesSync(DisposeCapability* capability, Result<void> completion) {
  std::vector<DisposableResource> stack = std::move(capability->stack);
  capability->stack.clear();
  ValueVector keepAlive;
  for (auto& r : stack) {
    keepAlive.push_back(r.resourceValue);
    keepAlive.push_back(r.disposeMethod);
  }
  for (size_t i = stack.size(); i-- > 0;) {
    const DisposableResource& r = stack[i];
    if (r.disposeMethod.isUndefined()) continue;
    Result<Value> result = Call(r.disposeMethod, r.resourceValue);
    if (result.isThrow()) {
      if (completion.isThrow()) completion = Throw{Value(NewSuppressedError(result.thrown(), completion.thrown()))};
      else completion = Throw{result.thrown()};
    }
  }
  return completion;
}

class DisposableStackObject : public Object {
 public:
  DisposableStackObject(Object* prototype) : Object(prototype), capability(make<DisposeCapability>()) {}
  bool disposed = false;
  DisposeCapability* capability;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(capability);
  }
};

Result<DisposableStackObject*> thisStack(Value v, const char* method) {
  auto* s = v.isObject() ? v.asObject()->as<DisposableStackObject>() : nullptr;
  if (!s) return ThrowTypeError(std::string("DisposableStack.prototype.") + method + " called on incompatible receiver");
  return s;
}

Result<DisposableStackObject*> livingStack(Value v, const char* method) {
  DisposableStackObject* s = TRY(thisStack(v, method));
  if (s->disposed) return ThrowReferenceError("DisposableStack has already been disposed");
  return s;
}

// DisposableStack ( )
Result<Value> DisposableStack_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor DisposableStack requires 'new'");
  return Value(TRY(OrdinaryCreateFromConstructor<DisposableStackObject>(call.newTarget, Intrinsic::DisposableStackPrototype)));
}

Result<Value> DisposableStackPrototype_disposed(NativeCall& call) {
  DisposableStackObject* s = TRY(thisStack(call.thisValue, "disposed"));
  return boolean(s->disposed);
}

Result<Value> DisposableStackPrototype_dispose(NativeCall& call) {
  DisposableStackObject* s = TRY(thisStack(call.thisValue, "dispose"));
  if (s->disposed) return Value::undefined();
  s->disposed = true;
  TRY(DisposeResourcesSync(s->capability, Result<void>()));
  return Value::undefined();
}

Result<Value> DisposableStackPrototype_use(NativeCall& call) {
  DisposableStackObject* s = TRY(livingStack(call.thisValue, "use"));
  TRY(AddDisposableResource(s->capability, call.args[0], DisposeHint::Sync));
  return call.args[0];
}

// The closure adopt registers: calls onDispose(value). slots: [onDispose, value].
Result<Value> adoptClosure(NativeCall& call) {
  return Call(call.callee->slots[0], Value::undefined(), {call.callee->slots[1]});
}

Result<Value> DisposableStackPrototype_adopt(NativeCall& call) {
  DisposableStackObject* s = TRY(livingStack(call.thisValue, "adopt"));
  if (!IsCallable(call.args[1])) return ThrowTypeError("DisposableStack.prototype.adopt: onDispose is not a function");
  BuiltinFunction* f = createBuiltinFunction(*currentRealm(), adoptClosure, 0, PropertyKey(u""));
  f->slots = {call.args[1], call.args[0]};
  TRY(AddDisposableResource(s->capability, Value::undefined(), DisposeHint::Sync, Value(f)));
  return call.args[0];
}

Result<Value> DisposableStackPrototype_defer(NativeCall& call) {
  DisposableStackObject* s = TRY(livingStack(call.thisValue, "defer"));
  if (!IsCallable(call.args[0])) return ThrowTypeError("DisposableStack.prototype.defer: onDispose is not a function");
  TRY(AddDisposableResource(s->capability, Value::undefined(), DisposeHint::Sync, call.args[0]));
  return Value::undefined();
}

Result<Value> DisposableStackPrototype_move(NativeCall& call) {
  DisposableStackObject* s = TRY(livingStack(call.thisValue, "move"));
  DisposableStackObject* moved =
      TRY(OrdinaryCreateFromConstructor<DisposableStackObject>(intrinsic(Intrinsic::DisposableStack), Intrinsic::DisposableStackPrototype));
  moved->capability = s->capability;
  s->capability = make<DisposeCapability>();
  s->disposed = true;
  return Value(moved);
}

// %Iterator.prototype% [ @@dispose ] ( )
Result<Value> IteratorPrototype_dispose(NativeCall& call) {
  Value o = call.thisValue;
  Value returnMethod = TRY(GetMethod(o, PropertyKey(u"return")));
  if (!returnMethod.isUndefined()) TRY(Call(returnMethod, o));
  return Value::undefined();
}

void installDisposableStack(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  defineConstructor(realm, Intrinsic::DisposableStack, DisposableStack_construct, 0, PropertyKey(u"DisposableStack"),
                    proto, Intrinsic::DisposableStackPrototype);
  defineFunction(realm, proto, PropertyKey(u"adopt"), 2, DisposableStackPrototype_adopt);
  defineFunction(realm, proto, PropertyKey(u"defer"), 1, DisposableStackPrototype_defer);
  BuiltinFunction* dispose = defineFunction(realm, proto, PropertyKey(u"dispose"), 0, DisposableStackPrototype_dispose);
  defineAccessor(realm, proto, PropertyKey(u"disposed"), DisposableStackPrototype_disposed, nullptr);
  defineFunction(realm, proto, PropertyKey(u"move"), 0, DisposableStackPrototype_move);
  defineFunction(realm, proto, PropertyKey(u"use"), 1, DisposableStackPrototype_use);
  defineValue(proto, PropertyKey(symbols().dispose), dispose);
  defineValue(proto, PropertyKey(symbols().toStringTag), str(u"DisposableStack"), Attr::Configurable);

  defineFunction(realm, realm.intrinsic(Intrinsic::IteratorPrototype), PropertyKey(symbols().dispose), 0, IteratorPrototype_dispose);
}

}  // namespace

SNOWMAN_BUILTIN(DisposableStack, kOrderDefault, installDisposableStack);

}  // namespace snowman::builtins
