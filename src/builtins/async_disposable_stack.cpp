// AsyncDisposableStack (explicit resource management) and %AsyncIteratorPrototype% [ @@asyncDispose ].
// disposeAsync awaits, so its DisposeResources runs on a native async body (interp/suspend.h).
#include "builtins/disposable.h"
#include "builtins/util.h"
#include "interp/dispose.h"
#include "interp/promise.h"
#include "interp/suspend.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

using interp::PromiseCapability;

class AsyncDisposableStackObject final : public Object {
 public:
  explicit AsyncDisposableStackObject(Object* prototype) : Object(prototype), capability(make<DisposeCapability>()) {}
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(capability);
  }

  bool disposed = false;  // [[AsyncDisposableState]]
  DisposeCapability* capability;
};

AsyncDisposableStackObject* asAsyncStack(Value v) {
  return v.isObject() ? v.asObject()->as<AsyncDisposableStackObject>() : nullptr;
}

Result<AsyncDisposableStackObject*> thisStack(Value v, const char* method) {
  AsyncDisposableStackObject* s = asAsyncStack(v);
  if (!s) {
    return ThrowTypeError(std::string("AsyncDisposableStack.prototype.") + method + " called on incompatible receiver");
  }
  return s;
}

Result<AsyncDisposableStackObject*> livingStack(Value v, const char* method) {
  AsyncDisposableStackObject* s = TRY(thisStack(v, method));
  if (s->disposed) return ThrowReferenceError("AsyncDisposableStack has already been disposed");
  return s;
}

PromiseCapability* newIntrinsicCapability() {
  return interp::NewPromiseCapability(Value(intrinsic(Intrinsic::Promise))).release();
}

// AsyncDisposableStack ( )
Result<Value> AsyncDisposableStack_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor AsyncDisposableStack requires 'new'");
  return Value(TRY(OrdinaryCreateFromConstructor<AsyncDisposableStackObject>(
      call.newTarget, Intrinsic::AsyncDisposableStackPrototype)));
}

// The closure adopt registers: calls onDisposeAsync(value). slots: [onDisposeAsync, value].
Result<Value> adoptClosure(NativeCall& call) {
  return Call(call.callee->slots[0], Value::undefined(), {call.callee->slots[1]});
}

Result<Value> AsyncDisposableStackPrototype_adopt(NativeCall& call) {
  AsyncDisposableStackObject* s = TRY(livingStack(call.thisValue, "adopt"));
  if (!IsCallable(call.args[1])) {
    return ThrowTypeError("AsyncDisposableStack.prototype.adopt: onDisposeAsync is not a function");
  }
  BuiltinFunction* f = createBuiltinFunction(*currentRealm(), adoptClosure, 0, PropertyKey(emptyString()));
  f->slots = {call.args[1], call.args[0]};
  TRY(interp::AddDisposableResource(s->capability, Value::undefined(), DisposeHint::Async, Value(f)));
  return call.args[0];
}

Result<Value> AsyncDisposableStackPrototype_defer(NativeCall& call) {
  AsyncDisposableStackObject* s = TRY(livingStack(call.thisValue, "defer"));
  if (!IsCallable(call.args[0])) {
    return ThrowTypeError("AsyncDisposableStack.prototype.defer: onDisposeAsync is not a function");
  }
  TRY(interp::AddDisposableResource(s->capability, Value::undefined(), DisposeHint::Async, call.args[0]));
  return Value::undefined();
}

// disposeAsync's DisposeResources(O.[[DisposeCapability]], NormalCompletion(undefined)); data: the capability.
Result<Value> disposeAsyncBody(interp::SuspendableObject* self) {
  auto* capability = static_cast<DisposeCapability*>(self->nativeData);
  Value thrown = interp::DisposeResourcesCore(capability, Value::empty());
  if (!thrown.isEmpty()) return Throw{thrown};
  return Value::undefined();
}

Result<Value> AsyncDisposableStackPrototype_disposeAsync(NativeCall& call) {
  PromiseCapability* promiseCapability = newIntrinsicCapability();
  AsyncDisposableStackObject* s = asAsyncStack(call.thisValue);
  if (!s) {
    Value error(CreateError(currentRealm(), Intrinsic::TypeErrorPrototype,
                            "AsyncDisposableStack.prototype.disposeAsync called on incompatible receiver"));
    TRY(interp::rejectCapability(promiseCapability, error));
    return promiseCapability->promise;
  }
  if (s->disposed) {
    TRY(interp::resolveCapability(promiseCapability, Value::undefined()));
    return promiseCapability->promise;
  }
  s->disposed = true;
  interp::InterpContext context;
  context.realm = currentRealm();
  context.function = call.callee;
  interp::AsyncNativeBlockStart(promiseCapability, disposeAsyncBody, s->capability, context);
  return promiseCapability->promise;
}

Result<Value> AsyncDisposableStackPrototype_disposed(NativeCall& call) {
  AsyncDisposableStackObject* s = TRY(thisStack(call.thisValue, "disposed"));
  return boolean(s->disposed);
}

Result<Value> AsyncDisposableStackPrototype_move(NativeCall& call) {
  AsyncDisposableStackObject* s = TRY(livingStack(call.thisValue, "move"));
  AsyncDisposableStackObject* moved = TRY(OrdinaryCreateFromConstructor<AsyncDisposableStackObject>(
      intrinsic(Intrinsic::AsyncDisposableStack), Intrinsic::AsyncDisposableStackPrototype));
  moved->capability = s->capability;
  s->capability = make<DisposeCapability>();
  s->disposed = true;
  return Value(moved);
}

Result<Value> AsyncDisposableStackPrototype_use(NativeCall& call) {
  AsyncDisposableStackObject* s = TRY(livingStack(call.thisValue, "use"));
  TRY(interp::AddDisposableResource(s->capability, call.args[0], DisposeHint::Async));
  return call.args[0];
}

// %AsyncIteratorPrototype% [ @@asyncDispose ]'s unwrap closure.
Result<Value> unwrapToUndefined(NativeCall&) { return Value::undefined(); }

// %AsyncIteratorPrototype% [ @@asyncDispose ] ( )
Result<Value> AsyncIteratorPrototype_asyncDispose(NativeCall& call) {
  Value O = call.thisValue;
  PromiseCapability* promiseCapability = newIntrinsicCapability();
  Result<Value> returnMethod = GetMethod(O, PropertyKey(u"return"));
  if (returnMethod.isThrow()) {
    TRY(interp::rejectCapability(promiseCapability, returnMethod.thrown()));
    return promiseCapability->promise;
  }
  if (returnMethod.value().isUndefined()) {
    TRY(interp::resolveCapability(promiseCapability, Value::undefined()));
    return promiseCapability->promise;
  }
  Result<Value> result = Call(returnMethod.value(), O);
  if (result.isNormal()) result = interp::PromiseResolve(intrinsic(Intrinsic::Promise), result.value());
  if (result.isThrow()) {
    TRY(interp::rejectCapability(promiseCapability, result.thrown()));
    return promiseCapability->promise;
  }
  BuiltinFunction* onFulfilled = createBuiltinFunction(*currentRealm(), unwrapToUndefined, 1, PropertyKey(emptyString()));
  interp::PerformPromiseThen(result.value().asObject()->as<interp::PromiseObject>(), Value(onFulfilled),
                             Value::undefined(), promiseCapability);
  return promiseCapability->promise;
}

void installAsyncDisposableStack(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  defineConstructor(realm, Intrinsic::AsyncDisposableStack, AsyncDisposableStack_construct, 0,
                    PropertyKey(u"AsyncDisposableStack"), proto, Intrinsic::AsyncDisposableStackPrototype);
  defineFunction(realm, proto, PropertyKey(u"adopt"), 2, AsyncDisposableStackPrototype_adopt);
  defineFunction(realm, proto, PropertyKey(u"defer"), 1, AsyncDisposableStackPrototype_defer);
  BuiltinFunction* disposeAsync =
      defineFunction(realm, proto, PropertyKey(u"disposeAsync"), 0, AsyncDisposableStackPrototype_disposeAsync);
  defineAccessor(realm, proto, PropertyKey(u"disposed"), AsyncDisposableStackPrototype_disposed, nullptr);
  defineFunction(realm, proto, PropertyKey(u"move"), 0, AsyncDisposableStackPrototype_move);
  defineFunction(realm, proto, PropertyKey(u"use"), 1, AsyncDisposableStackPrototype_use);
  defineValue(proto, PropertyKey(symbols().asyncDispose), Value(disposeAsync));
  defineValue(proto, PropertyKey(symbols().toStringTag), str(u"AsyncDisposableStack"), Attr::Configurable);

  defineFunction(realm, realm.intrinsic(Intrinsic::AsyncIteratorPrototype), PropertyKey(symbols().asyncDispose), 0,
                 AsyncIteratorPrototype_asyncDispose);
}

}  // namespace

SNOWMAN_BUILTIN(AsyncDisposableStack, kOrderLate, installAsyncDisposableStack);

}  // namespace snowman::builtins
