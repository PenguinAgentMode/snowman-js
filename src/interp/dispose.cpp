// Explicit resource management in the evaluator (see dispose.h).
#include "interp/dispose.h"

#include "interp/promise.h"
#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/vm.h"

namespace snowman::interp {

using builtins::DisposableResource;
using builtins::DisposeCapability;
using builtins::DisposeHint;

namespace {

// GetDisposeMethod step 1.b.ii's closure: calls the @@dispose method (slots[0]) on the this value and
// returns a promise for undefined, or rejected with what it threw.
Result<Value> asyncFromSyncDispose(NativeCall& call) {
  Realm* realm = vm().currentRealm();
  PromiseCapability* capability = NewPromiseCapability(Value(realm->intrinsic(Intrinsic::Promise))).release();
  Result<Value> result = Call(call.callee->slots[0], call.thisValue);
  if (result.isThrow()) {
    TRY(rejectCapability(capability, result.thrown()));
    return capability->promise;
  }
  TRY(resolveCapability(capability, Value::undefined()));
  return capability->promise;
}

// GetDisposeMethod(V, async-dispose)
Result<Value> GetAsyncDisposeMethod(Value value) {
  Value method = TRY(GetMethod(value, PropertyKey(vm().symbols.asyncDispose)));
  if (!method.isUndefined()) return method;
  method = TRY(GetMethod(value, PropertyKey(vm().symbols.dispose)));
  if (method.isUndefined()) return method;
  BuiltinFunction* closure =
      createBuiltinFunction(*vm().currentRealm(), asyncFromSyncDispose, 0, PropertyKey(emptyString()));
  closure->slots.push_back(method);
  return Value(closure);
}

}  // namespace

Result<void> AddDisposableResource(DisposeCapability* capability, Value value, DisposeHint hint, Value method) {
  if (hint == DisposeHint::Sync || !method.isEmpty()) {
    return builtins::AddDisposableResource(capability, value, hint, method);
  }
  // CreateDisposableResource(V, async-dispose)
  if (value.isNullish()) {
    // Recorded so that leaving the scope still awaits.
    capability->stack.push_back({Value::undefined(), hint, Value::undefined()});
    return {};
  }
  if (!value.isObject()) return ThrowTypeError("await using: the resource is not an object");
  Value disposeMethod = TRY(GetAsyncDisposeMethod(value));
  if (disposeMethod.isUndefined()) {
    return ThrowTypeError("await using: the resource has no Symbol.asyncDispose or Symbol.dispose method");
  }
  capability->stack.push_back({value, hint, disposeMethod});
  return {};
}

Value DisposeResourcesCore(DisposeCapability* capability, Value thrown) {
  bool needsAwait = false;
  bool hasAwaited = false;
  // The resources stay in the (traced) capability until the end; nothing adds to it while it is disposed.
  for (size_t i = capability->stack.size(); i-- > 0;) {
    if (i >= capability->stack.size()) continue;
    DisposableResource resource = capability->stack[i];
    if (resource.hint == DisposeHint::Sync && needsAwait && !hasAwaited) {
      (void)Await(Value::undefined());
      needsAwait = false;
    }
    if (resource.disposeMethod.isUndefined()) {
      needsAwait = true;  // an `await using` of null or undefined
      continue;
    }
    Result<Value> result = Call(resource.disposeMethod, resource.resourceValue);
    if (result.isNormal() && resource.hint == DisposeHint::Async) {
      result = Await(result.value());
      hasAwaited = true;
    }
    if (result.isThrow()) {
      if (thrown.isEmpty()) {
        thrown = result.thrown();
      } else {
        thrown = Value(builtins::NewSuppressedError(result.thrown(), thrown));
      }
    }
  }
  if (needsAwait && !hasAwaited) (void)Await(Value::undefined());
  capability->stack.clear();
  return thrown;
}

Result<void> addUsingResource(Environment* env, Value value, ast::VariableKind kind) {
  auto* declarative = static_cast<DeclarativeEnvironment*>(env);
  if (!declarative->disposeCapability) declarative->disposeCapability = snowman::make<DisposeCapability>();
  DisposeHint hint = kind == ast::VariableKind::AwaitUsing ? DisposeHint::Async : DisposeHint::Sync;
  return interp::AddDisposableResource(declarative->disposeCapability, value, hint);
}

DisposeCapability* pendingDisposeCapability(Environment* env) {
  switch (env->kind()) {
    case Environment::Kind::Declarative:
    case Environment::Kind::Function:
    case Environment::Kind::Module: {
      DisposeCapability* capability = static_cast<DeclarativeEnvironment*>(env)->disposeCapability;
      return capability && !capability->stack.empty() ? capability : nullptr;
    }
    default:
      return nullptr;
  }
}

}  // namespace snowman::interp
