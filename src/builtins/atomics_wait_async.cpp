// Atomics.waitAsync ( typedArray, index, value, timeout ) (25.4.14) and the Atomics.notify that wakes its
// waiters (25.4.15), for a single agent: every waiter is an async one of this agent, so NotifyWaiter resolves
// its promise directly, and a timeout is HostEnqueueTimeoutJob run from the job queue. Replaces the
// Atomics.notify atomics.cpp installs, which can find no waiters.
#include <time.h>

#include <algorithm>
#include <cmath>
#include <list>
#include <map>

#include "builtins/util.h"
#include "interp/promise.h"
#include "runtime/arraybuffer.h"
#include "runtime/bigint.h"
#include "runtime/typedarray.h"

namespace snowman::builtins {
namespace {

double monotonicMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) * 1000 + double(ts.tv_nsec) / 1e6;
}

// A Waiter Record (25.4.3.x) of this agent: always async (single agent: nothing can block).
struct Waiter {
  interp::PromiseCapability* capability;
  double timeoutTime;  // monotonic ms; +inf: none
  const char16_t* result = u"ok";
};

// The WaiterLists, by (Shared Data Block, byte index): a SharedArrayBuffer is its block's only owner here.
// Rooted: the buffers and the waiters' promise capabilities.
class WaiterLists : public RootProvider {
 public:
  using Key = std::pair<ArrayBufferObject*, size_t>;
  std::map<Key, std::list<Waiter*>> lists;
  bool timerQueued = false;

  void traceRoots(Tracer& tracer) override {
    for (auto& [key, list] : lists) {
      tracer.visit(reinterpret_cast<Cell*>(key.first));
      for (Waiter* w : list) tracer.visit(reinterpret_cast<Cell*>(w->capability));
    }
  }

  // The earliest finite timeout among the waiters, or +inf.
  double nextTimeout() const {
    double t = INFINITY;
    for (auto& [key, list] : lists) {
      for (Waiter* w : list) t = std::min(t, w->timeoutTime);
    }
    return t;
  }
};

WaiterLists& waiterLists() {
  static WaiterLists* lists = new WaiterLists();
  return *lists;
}

// NotifyWaiter for a waiter of this agent: Call(promiseCapability.[[Resolve]], undefined, « result »).
Result<void> notifyWaiter(Waiter* w) {
  Result<void> r = interp::resolveCapability(w->capability, str(w->result));
  delete w;
  return r;
}

// The timeout jobs of all waiters (HostEnqueueTimeoutJob), as one job: it runs every timeout that is due, in
// timeout order, then puts itself back on the queue while any finite timeout remains, sleeping first when
// nothing else is queued.
class TimeoutJob final : public Job {
 public:
  explicit TimeoutJob(Realm* realm) : Job(realm) {}
  Result<void> run() override {
    WaiterLists& wl = waiterLists();
    wl.timerQueued = false;
    while (true) {
      double now = monotonicMs();
      Waiter* due = nullptr;
      std::list<Waiter*>* dueList = nullptr;
      for (auto& [key, list] : wl.lists) {
        for (Waiter* w : list) {
          if (w->timeoutTime <= now && (!due || w->timeoutTime < due->timeoutTime)) {
            due = w;
            dueList = &list;
          }
        }
      }
      if (!due) break;
      due->result = u"timed-out";
      dueList->remove(due);
      TRY(notifyWaiter(due));
    }
    for (auto it = wl.lists.begin(); it != wl.lists.end();) it = it->second.empty() ? wl.lists.erase(it) : std::next(it);
    double next = wl.nextTimeout();
    if (std::isfinite(next)) {
      if (!vm().hasJobs()) {
        double wait = next - monotonicMs();
        if (wait > 0) {
          struct timespec ts{time_t(wait / 1000), long(std::fmod(wait, 1000) * 1e6)};
          nanosleep(&ts, nullptr);
        }
      }
      scheduleTimeouts(realm);
    }
    return {};
  }
  static void scheduleTimeouts(Realm* realm) {
    WaiterLists& wl = waiterLists();
    if (wl.timerQueued) return;
    wl.timerQueued = true;
    vm().enqueueJob(make<TimeoutJob>(realm));
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
  Waiter* waiter = new Waiter{capability, monotonicMs() + t};
  waiterLists().lists[{buffer, byteIndex}].push_back(waiter);
  if (std::isfinite(t)) TimeoutJob::scheduleTimeouts(currentRealm());
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
  WaiterLists& wl = waiterLists();
  auto it = wl.lists.find({buffer, byteIndex});
  if (it == wl.lists.end()) return num(0);
  // RemoveWaiters(WL, c): the first c, in FIFO order; then NotifyWaiter each.
  std::vector<Waiter*> removed;
  while (!it->second.empty() && double(removed.size()) < c) {
    removed.push_back(it->second.front());
    it->second.pop_front();
  }
  if (it->second.empty()) wl.lists.erase(it);
  for (Waiter* w : removed) TRY(notifyWaiter(w));
  return num(double(removed.size()));
}

void installAtomicsWaitAsync(Realm& realm) {
  Object* atomics = realm.intrinsic(Intrinsic::Atomics);
  defineFunction(realm, atomics, PropertyKey(u"notify"), 3, Atomics_notify);
  defineFunction(realm, atomics, PropertyKey(u"waitAsync"), 4, Atomics_waitAsync);
}

}  // namespace

SNOWMAN_BUILTIN(AtomicsWaitAsync, kOrderLate, installAtomicsWaitAsync);

}  // namespace snowman::builtins
