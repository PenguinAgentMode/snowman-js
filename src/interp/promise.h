// Promise objects and the Promise abstract operations (27.2.1), for the interpreter (await, async functions,
// dynamic import) and the Promise built-in (src/builtins/promise.cpp, which implements them).
#pragma once

#include <vector>

#include "runtime/function.h"
#include "runtime/object.h"
#include "runtime/realm.h"
#include "runtime/vm.h"

namespace snowman::interp {

// A PromiseCapability Record. An object (never exposed) so built-in functions can hold it in their slots.
class PromiseCapability final : public Object {
 public:
  PromiseCapability() : Object(nullptr) {}
  void trace(Tracer& tracer) override;

  Value promise;
  Value resolve;
  Value reject;
};

// A PromiseReaction Record.
class PromiseReaction final : public Cell {
 public:
  enum class Type : uint8_t { Fulfill, Reject };
  PromiseReaction(PromiseCapability* capability, Type type, Value handler)
      : capability(capability), type(type), handler(handler) {}
  void trace(Tracer& tracer) override;

  PromiseCapability* capability;  // nullptr: undefined
  Type type;
  Value handler;  // a callable, or empty
};

class PromiseObject final : public Object {
 public:
  enum class State : uint8_t { Pending, Fulfilled, Rejected };
  explicit PromiseObject(Object* prototype) : Object(prototype) {}
  void trace(Tracer& tracer) override;

  State state = State::Pending;
  Value result;
  std::vector<PromiseReaction*> fulfillReactions;
  std::vector<PromiseReaction*> rejectReactions;
  bool isHandled = false;
};

bool IsPromise(Value value);
// NewPromiseCapability(C)
Result<PromiseCapability*> NewPromiseCapability(Value C);
// PromiseResolve(C, x)
Result<Value> PromiseResolve(Object* C, Value x);
// PerformPromiseThen(promise, onFulfilled, onRejected[, resultCapability]): undefined, or the result promise.
Value PerformPromiseThen(PromiseObject* promise, Value onFulfilled, Value onRejected,
                         PromiseCapability* resultCapability = nullptr);
// The resolving functions' behaviour without the functions (for promises the interpreter settles itself).
void FulfillPromise(PromiseObject* promise, Value value);
void RejectPromise(PromiseObject* promise, Value reason);
// Call(capability.[[Resolve]] / [[Reject]], undefined, « value »).
Result<void> resolveCapability(PromiseCapability* capability, Value value);
Result<void> rejectCapability(PromiseCapability* capability, Value reason);

// A job that calls a native function with arguments (HostEnqueuePromiseJob of an abstract closure).
class NativeJob final : public Job {
 public:
  using Fn = Result<void> (*)(NativeJob& job);
  NativeJob(Realm* realm, Fn fn) : Job(realm), fn(fn) {}
  Result<void> run() override;
  void trace(Tracer& tracer) override;

  Fn fn;
  std::vector<Value> values;
};

}  // namespace snowman::interp
