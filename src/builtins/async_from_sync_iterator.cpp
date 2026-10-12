// %AsyncFromSyncIteratorPrototype% and CreateAsyncFromSyncIterator (27.1.6).
#include "interp/promise.h"
#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/operations.h"

namespace snowman {

namespace {

using interp::PromiseCapability;

class AsyncFromSyncIterator final : public Object {
 public:
  AsyncFromSyncIterator(Object* prototype, const IteratorRecord& record) : Object(prototype), syncIteratorRecord(record) {}
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(syncIteratorRecord.iterator);
    tracer.visit(syncIteratorRecord.nextMethod);
  }
  IteratorRecord syncIteratorRecord;  // [[SyncIteratorRecord]]
};

// The record kept by the closures of AsyncFromSyncIteratorContinuation.
class IteratorHolder final : public Object {
 public:
  explicit IteratorHolder(const IteratorRecord& record) : Object(nullptr), record(record) {}
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(record.iterator);
    tracer.visit(record.nextMethod);
  }
  IteratorRecord record;
};

Realm* currentRealm() { return vm().currentRealm(); }

PromiseCapability* intrinsicCapability() {
  return interp::NewPromiseCapability(Value(currentRealm()->intrinsic(Intrinsic::Promise))).release();
}

Value rejectWith(PromiseCapability* capability, Value reason) {
  (void)interp::rejectCapability(capability, reason);
  return capability->promise;
}

Value typeError(const char* message) { return Value(CreateError(currentRealm(), Intrinsic::TypeErrorPrototype, message)); }

// slots: [done]
Result<Value> unwrap(NativeCall& call) {
  return Value(CreateIteratorResultObject(call.args[0], call.callee->slots[0].asBoolean()));
}

// slots: [holder]
Result<Value> closeIterator(NativeCall& call) {
  auto* holder = call.callee->slots[0].asObject()->as<IteratorHolder>();
  TRY(IteratorClose(holder->record, Throw{call.args[0]}));
  return Value::undefined();
}

// 27.1.6.4 AsyncFromSyncIteratorContinuation
Value AsyncFromSyncIteratorContinuation(Object* result, PromiseCapability* promiseCapability,
                                        const IteratorRecord& syncIteratorRecord, bool closeOnRejection) {
  Result<bool> done = IteratorComplete(result);
  if (done.isThrow()) return rejectWith(promiseCapability, done.thrown());
  Result<Value> value = IteratorValue(result);
  if (value.isThrow()) return rejectWith(promiseCapability, value.thrown());
  Result<Value> valueWrapper = interp::PromiseResolve(currentRealm()->intrinsic(Intrinsic::Promise), value.value());
  if (valueWrapper.isThrow() && !done.value() && closeOnRejection) {
    Result<void> closed = IteratorClose(syncIteratorRecord, Throw{valueWrapper.thrown()});
    valueWrapper = Throw{closed.thrown()};
  }
  if (valueWrapper.isThrow()) return rejectWith(promiseCapability, valueWrapper.thrown());
  BuiltinFunction* onFulfilled = createBuiltinFunction(*currentRealm(), unwrap, 1, PropertyKey(emptyString()));
  onFulfilled->slots.push_back(Value::boolean(done.value()));
  Value onRejected = Value::undefined();
  if (!done.value() && closeOnRejection) {
    BuiltinFunction* closer = createBuiltinFunction(*currentRealm(), closeIterator, 1, PropertyKey(emptyString()));
    closer->slots.push_back(Value(make<IteratorHolder>(syncIteratorRecord)));
    onRejected = Value(closer);
  }
  interp::PerformPromiseThen(valueWrapper.value().asObject()->as<interp::PromiseObject>(), Value(onFulfilled),
                             onRejected, promiseCapability);
  return promiseCapability->promise;
}

AsyncFromSyncIterator* thisIterator(NativeCall& call) {
  return call.thisValue.isObject() ? call.thisValue.asObject()->as<AsyncFromSyncIterator>() : nullptr;
}

// 27.1.6.2.1 %AsyncFromSyncIteratorPrototype%.next
Result<Value> asyncFromSyncNext(NativeCall& call) {
  AsyncFromSyncIterator* O = thisIterator(call);
  PromiseCapability* promiseCapability = intrinsicCapability();
  if (!O) return rejectWith(promiseCapability, typeError("not an async-from-sync iterator"));
  IteratorRecord& syncIteratorRecord = O->syncIteratorRecord;
  Result<Object*> result = call.args.empty() ? IteratorNext(syncIteratorRecord)
                                             : IteratorNext(syncIteratorRecord, call.args[0]);
  if (result.isThrow()) return rejectWith(promiseCapability, result.thrown());
  return AsyncFromSyncIteratorContinuation(result.value(), promiseCapability, syncIteratorRecord, true);
}

// 27.1.6.2.2 %AsyncFromSyncIteratorPrototype%.return
Result<Value> asyncFromSyncReturn(NativeCall& call) {
  AsyncFromSyncIterator* O = thisIterator(call);
  PromiseCapability* promiseCapability = intrinsicCapability();
  if (!O) return rejectWith(promiseCapability, typeError("not an async-from-sync iterator"));
  IteratorRecord& syncIteratorRecord = O->syncIteratorRecord;
  Object* syncIterator = syncIteratorRecord.iterator;
  Result<Value> returnMethod = GetMethod(Value(syncIterator), PropertyKey(u"return"));
  if (returnMethod.isThrow()) return rejectWith(promiseCapability, returnMethod.thrown());
  if (returnMethod.value().isUndefined()) {
    (void)interp::resolveCapability(promiseCapability, Value(CreateIteratorResultObject(call.args[0], true)));
    return promiseCapability->promise;
  }
  Result<Value> result = call.args.empty() ? Call(returnMethod.value(), Value(syncIterator))
                                           : Call(returnMethod.value(), Value(syncIterator), {call.args[0]});
  if (result.isThrow()) return rejectWith(promiseCapability, result.thrown());
  if (!result.value().isObject()) {
    return rejectWith(promiseCapability, typeError("iterator.return() did not return an object"));
  }
  return AsyncFromSyncIteratorContinuation(result.value().asObject(), promiseCapability, syncIteratorRecord, false);
}

// 27.1.6.2.3 %AsyncFromSyncIteratorPrototype%.throw
Result<Value> asyncFromSyncThrow(NativeCall& call) {
  AsyncFromSyncIterator* O = thisIterator(call);
  PromiseCapability* promiseCapability = intrinsicCapability();
  if (!O) return rejectWith(promiseCapability, typeError("not an async-from-sync iterator"));
  IteratorRecord& syncIteratorRecord = O->syncIteratorRecord;
  Object* syncIterator = syncIteratorRecord.iterator;
  Result<Value> throwMethod = GetMethod(Value(syncIterator), PropertyKey(u"throw"));
  if (throwMethod.isThrow()) return rejectWith(promiseCapability, throwMethod.thrown());
  if (throwMethod.value().isUndefined()) {
    Result<void> closed = IteratorClose(syncIteratorRecord, Result<void>());
    if (closed.isThrow()) return rejectWith(promiseCapability, closed.thrown());
    return rejectWith(promiseCapability, typeError("The iterator does not provide a 'throw' method"));
  }
  Result<Value> result = Call(throwMethod.value(), Value(syncIterator), {call.args[0]});
  if (result.isThrow()) return rejectWith(promiseCapability, result.thrown());
  if (!result.value().isObject()) {
    return rejectWith(promiseCapability, typeError("iterator.throw() did not return an object"));
  }
  return AsyncFromSyncIteratorContinuation(result.value().asObject(), promiseCapability, syncIteratorRecord, true);
}

void installAsyncFromSyncIteratorPrototype(Realm& realm) {
  Object* prototype = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::AsyncIteratorPrototype));
  realm.setIntrinsic(Intrinsic::AsyncFromSyncIteratorPrototype, prototype);
  defineFunction(realm, prototype, PropertyKey(u"next"), 1, asyncFromSyncNext);
  defineFunction(realm, prototype, PropertyKey(u"return"), 1, asyncFromSyncReturn);
  defineFunction(realm, prototype, PropertyKey(u"throw"), 1, asyncFromSyncThrow);
}

}  // namespace

// 27.1.6.1 CreateAsyncFromSyncIterator
Result<IteratorRecord> CreateAsyncFromSyncIterator(const IteratorRecord& syncIteratorRecord) {
  Object* prototype = vm().currentRealm()->intrinsic(Intrinsic::AsyncFromSyncIteratorPrototype);
  auto* asyncIterator = make<AsyncFromSyncIterator>(prototype, syncIteratorRecord);
  Value nextMethod = Get(asyncIterator, PropertyKey(u"next")).release();
  return IteratorRecord{asyncIterator, nextMethod, false};
}

SNOWMAN_BUILTIN(AsyncFromSyncIteratorPrototype, kOrderDefault, installAsyncFromSyncIteratorPrototype);

}  // namespace snowman
