// Explicit resource management in the evaluator: `using` / `await using` bindings add resources to their
// environment's [[DisposeCapability]], and a scope's end runs DisposeResources over it. The operations that may
// Await (await using, AsyncDisposableStack) live here; the sync-only ones are src/builtins/disposable.h.
#pragma once

#include "builtins/disposable.h"
#include "interp/interpreter.h"

namespace snowman::interp {

// AddDisposableResource(disposeCapability, V, hint [, method]) (method empty = not present), with
// GetDisposeMethod's async-dispose fallback to a wrapped @@dispose.
Result<void> AddDisposableResource(builtins::DisposeCapability* capability, Value value, builtins::DisposeHint hint,
                                   Value method = Value::empty());

// The DisposeResources loop over capability, awaiting async-dispose results, then empties it. `thrown` is the
// completion's thrown value, empty when it is not a throw completion (a generator's return marker is not one).
// Returns the thrown value of the resulting completion, empty when the completion stands as it was.
Value DisposeResourcesCore(builtins::DisposeCapability* capability, Value thrown);

// A `using` (sync-dispose) / `await using` (async-dispose) binding of env initialized to value.
Result<void> addUsingResource(Environment* env, Value value, ast::VariableKind kind);

// The environment's [[DisposeCapability]] when it holds resources, else nullptr.
builtins::DisposeCapability* pendingDisposeCapability(Environment* env);

// DisposeResources(env.[[DisposeCapability]], completion) at the end of env's scope.
template <class T>
Result<T> DisposeResources(Environment* env, Result<T> completion) {
  builtins::DisposeCapability* capability = pendingDisposeCapability(env);
  if (!capability) return completion;
  Value thrown = completion.isThrow() ? completion.thrown() : Value::empty();
  Value result = DisposeResourcesCore(capability, thrown);
  if (result.isEmpty()) return completion;
  return Throw{result};
}

}  // namespace snowman::interp
