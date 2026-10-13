// Atomics.waitAsync ( typedArray, index, value, timeout ) (25.4.14) and Atomics.notify (25.4.15), over the
// process-wide waiter lists of interp/agent.cpp: a notify wakes sync waiters of any agent, resolves this thread's
// async waiters directly and posts other agents' to their mailboxes; timeouts and those wakeups are a job on each
// thread's own queue. Replaces the Atomics.notify atomics.cpp installs.
#include <time.h>

#include <algorithm>
#include <cmath>
#include <list>
#include <map>

#include "builtins/util.h"
#include "interp/agent.h"
#include "interp/promise.h"
#include "runtime/arraybuffer.h"
#include "runtime/bigint.h"
#include "runtime/typedarray.h"

namespace snowman::builtins {
namespace {

// An async waiter of this thread (agent): its promise capability lives in this thread's heap; the process-wide
// waiter lists (interp/agent.cpp) hold only its id, so a notify from another agent comes back as a mailbox wakeup.
struct Waiter {
  interp::PromiseCapability* capability;
  double timeoutTime;  // monotonic ms; +inf: none
  const SharedBlock* block;
  size_t byteIndex;
};

// This thread's async waiters by id. Rooted: their promise capabilities.
class WaiterTable : public RootProvider {
 public:
  std::map<uint64_t, Waiter> waiters;
  bool pumpQueued = false;
  void traceRoots(Tracer& tracer) override {
    for (auto& [id, w] : waiters) tracer.visit(reinterpret_cast<Cell*>(w.capability));
  }
  double nextTimeout() const {
    double t = INFINITY;
    for (auto& [id, w] : waiters) t = std::min(t, w.timeoutTime);
    return t;
  }
};

WaiterTable& waiterTable() {
  thread_local WaiterTable* table = new WaiterTable();
  return *table;
}

// NotifyWaiter: Call(promiseCapability.[[Resolve]], undefined, « result »).
Result<void> settle(uint64_t id, const char16_t* result) {
  WaiterTable& table = waiterTable();
  auto it = table.waiters.find(id);
  if (it == table.waiters.end()) return {};
  interp::PromiseCapability* capability = it->second.capability;
  table.waiters.erase(it);
  return interp::resolveCapability(capability, str(result));
}

// The timeouts and cross-agent wakeups of this thread's async waiters (HostEnqueueTimeoutJob), as one job that puts
// itself back on the queue while a waiter can still be settled, blocking on the mailbox when nothing else is queued.
class PumpJob final : public Job {
 public:
  explicit PumpJob(Realm* realm) : Job(realm) {}
  Result<void> run() override {
    WaiterTable& table = waiterTable();
    table.pumpQueued = false;
    for (uint64_t id : interp::TakeAsyncWakeups()) TRY(settle(id, u"ok"));
    while (true) {
      double now = interp::MonotonicMs();
      uint64_t due = 0;
      double dueTime = INFINITY;
      for (auto& [id, w] : table.waiters) {
        if (w.timeoutTime <= now && w.timeoutTime < dueTime) {
          due = id;
          dueTime = w.timeoutTime;
        }
      }
      if (!due) break;
      Waiter w = table.waiters[due];
      if (interp::RemoveAsyncWaiter(w.block, w.byteIndex, due)) {
        TRY(settle(due, u"timed-out"));
      } else {
        table.waiters[due].timeoutTime = INFINITY;  // a notify took it first: its wakeup is on the way
      }
    }
    double next = table.nextTimeout();
    if (!table.waiters.empty() && (std::isfinite(next) || interp::AgentThreadsRunning())) {
      if (!vm().hasJobs()) interp::WaitForMailbox(std::min(next, interp::MonotonicMs() + 100));
      schedule(realm);
    }
    return {};
  }
  static void schedule(Realm* realm) {
    WaiterTable& table = waiterTable();
    if (table.pumpQueued) return;
    table.pumpQueued = true;
    vm().enqueueJob(make<PumpJob>(realm));
  }
};

// 25.4.3.1 ValidateIntegerTypedArray ( typedArray, true ) and 25.4.3.2 ValidateAtomicAccess: the byte index.
Result<TypedArrayObject*> ValidateWaitableTypedArray(Value typedArray) {
  TypedArrayObject* ta = TRY(ValidateTypedArray(typedArray));
  if (ta->elementType != ElementType::Int32 && ta->elementType != ElementType::BigInt64)
    return ThrowTypeError("Atomics: typed array must be an Int32Array or BigInt64Array");
  return ta;
}
Result<size_t> ValidateAtomicAccess(TypedArrayObject* ta, Value requestIndex) {
  double length = TypedArrayLength(ta);
  double accessIndex = TRY(ToIndex(requestIndex));
  if (accessIndex >= length) return ThrowRangeError("Atomics: index out of range");
  return size_t(accessIndex) * ElementSize(ta->elementType) + size_t(ta->byteOffset);
}

Value waitResult(bool async, Value value) {
  Object* result = OrdinaryObjectCreate(intrinsic(Intrinsic::ObjectPrototype));
  (void)CreateDataPropertyOrThrow(result, PropertyKey(u"async"), boolean(async));
  (void)CreateDataPropertyOrThrow(result, PropertyKey(u"value"), value);
  return Value(result);
}

// 25.4.3.14 DoWait ( async, typedArray, index, value, timeout )
Result<Value> Atomics_waitAsync(NativeCall& call) {
  TypedArrayObject* ta = TRY(ValidateWaitableTypedArray(call.args[0]));
  ArrayBufferObject* buffer = ta->buffer;
  if (!buffer->isShared()) return ThrowTypeError("Atomics.waitAsync requires a shared typed array");
  size_t byteIndex = TRY(ValidateAtomicAccess(ta, call.args[1]));
  Value v;
  if (ta->elementType == ElementType::BigInt64) {
    v = Value(BigInt::fromInt64(TRY(ToBigInt64(call.args[2]))));
  } else {
    v = num(TRY(ToInt32(call.args[2])));
  }
  double q = TRY(ToNumber(call.args[3]));
  double t = std::isnan(q) || q == INFINITY ? INFINITY : q == -INFINITY ? 0 : std::max(q, 0.0);
  interp::PromiseCapability* capability = TRY(interp::NewPromiseCapability(Value(intrinsic(Intrinsic::Promise))));
  Value w = GetValueFromBuffer(buffer, byteIndex, ta->elementType);
  bool equal = v.isBigInt() ? BigInt::equals(v.asBigInt(), w.asBigInt()) : v.asNumber() == w.asNumber();
  if (!equal) return waitResult(false, str(u"not-equal"));
  // Asynchronous immediate timeouts fail fast, without a promise job.
  if (t == 0) return waitResult(false, str(u"timed-out"));
  // Registered under the process-wide waiter-list lock, which reads the element again.
  const SharedBlock* block = buffer->sharedBlock().get();
  auto stillEqual = [&] {
    Value now = GetValueFromBuffer(buffer, byteIndex, ta->elementType);
    return v.isBigInt() ? BigInt::equals(v.asBigInt(), now.asBigInt()) : v.asNumber() == now.asNumber();
  };
  uint64_t id = interp::AddAsyncWaiter(block, byteIndex, stillEqual);
  if (!id) return waitResult(false, str(u"not-equal"));
  waiterTable().waiters[id] = Waiter{capability, interp::MonotonicMs() + t, block, byteIndex};
  PumpJob::schedule(currentRealm());
  return waitResult(true, capability->promise);
}

// 25.4.15 Atomics.notify ( typedArray, index, count )
Result<Value> Atomics_notify(NativeCall& call) {
  TypedArrayObject* ta = TRY(ValidateWaitableTypedArray(call.args[0]));
  size_t byteIndex = TRY(ValidateAtomicAccess(ta, call.args[1]));
  double c = INFINITY;
  if (!call.args[2].isUndefined()) c = std::max(TRY(ToIntegerOrInfinity(call.args[2])), 0.0);
  ArrayBufferObject* buffer = ta->buffer;
  if (!buffer->isShared()) return num(0);
  // RemoveWaiters(WL, c) in FIFO order, then NotifyWaiter each: other agents' waiters wake in their own threads.
  std::vector<uint64_t> own;
  size_t woken = interp::NotifyWaiters(buffer->sharedBlock().get(), byteIndex, c, own);
  for (uint64_t id : own) TRY(settle(id, u"ok"));
  return num(double(woken));
}

void installAtomicsWaitAsync(Realm& realm) {
  Object* atomics = realm.intrinsic(Intrinsic::Atomics);
  defineFunction(realm, atomics, PropertyKey(u"notify"), 3, Atomics_notify);
  defineFunction(realm, atomics, PropertyKey(u"waitAsync"), 4, Atomics_waitAsync);
}

}  // namespace

SNOWMAN_BUILTIN(AtomicsWaitAsync, kOrderLate, installAtomicsWaitAsync);

}  // namespace snowman::builtins
