// Array.fromAsync ( asyncItems [ , mapfn [ , thisArg ] ] ). Its closure awaits, so it runs on a native async
// body (interp/suspend.h) started like AsyncFunctionStart.
#include "builtins/util.h"
#include "interp/promise.h"
#include "interp/suspend.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

// What fromAsyncClosure captures: C, asyncItems, mapfn and thisArg.
class FromAsyncState final : public Cell {
 public:
  void trace(Tracer& tracer) override {
    tracer.visit(constructor);
    tracer.visit(asyncItems);
    tracer.visit(mapfn);
    tracer.visit(thisArg);
  }

  Value constructor;
  Value asyncItems;
  Value mapfn;
  Value thisArg;
};

// IfAbruptCloseAsyncIterator(value, iteratorRecord)
template <class T>
Result<T> closeIfAbrupt(const IteratorRecord& record, Result<T> value) {
  if (value.isNormal()) return value;
  TRY(interp::AsyncIteratorClose(record, Throw{value.thrown()}));
  return Throw{value.thrown()};
}

Result<Value> fromAsyncIterator(FromAsyncState* state, IteratorRecord iteratorRecord, bool mapping) {
  Object* A;
  if (IsConstructor(state->constructor)) {
    A = TRY(Construct(state->constructor.asObject(), {}));
  } else {
    A = TRY(ArrayCreate(0));
  }
  Rooted<Object> rootA(A);
  for (double k = 0;; k++) {
    if (k >= 9007199254740991.0) {
      Result<void> error = ThrowTypeError("Array.fromAsync: too many elements");
      TRY(interp::AsyncIteratorClose(iteratorRecord, error));
      return Throw{error.thrown()};
    }
    Value nextResult = TRY(Call(iteratorRecord.nextMethod, Value(iteratorRecord.iterator)));
    nextResult = TRY(interp::Await(nextResult));
    if (!nextResult.isObject()) return ThrowTypeError("Array.fromAsync: iterator result is not an object");
    bool done = TRY(IteratorComplete(nextResult.asObject()));
    if (done) {
      TRY(Set(A, PropertyKey(u"length"), Value::number(k), true));
      return Value(A);
    }
    Value nextValue = TRY(IteratorValue(nextResult.asObject()));
    Value mappedValue = nextValue;
    if (mapping) {
      mappedValue = TRY(closeIfAbrupt(iteratorRecord, Call(state->mapfn, state->thisArg, {nextValue, Value::number(k)})));
      mappedValue = TRY(closeIfAbrupt(iteratorRecord, interp::Await(mappedValue)));
    }
    Result<void> defineStatus = CreateDataPropertyOrThrow(A, indexKey(k), mappedValue);
    if (defineStatus.isThrow()) {
      TRY(interp::AsyncIteratorClose(iteratorRecord, defineStatus));
      return Throw{defineStatus.thrown()};
    }
  }
}

Result<Value> fromAsyncArrayLike(FromAsyncState* state, bool mapping) {
  Object* arrayLike = TRY(ToObject(state->asyncItems));
  double len = TRY(LengthOfArrayLike(arrayLike));
  Object* A;
  if (IsConstructor(state->constructor)) {
    A = TRY(Construct(state->constructor.asObject(), {Value::number(len)}));
  } else {
    A = TRY(ArrayCreate(len));
  }
  Rooted<Object> rootA(A);
  for (double k = 0; k < len; k++) {
    Value kValue = TRY(Get(arrayLike, indexKey(k)));
    kValue = TRY(interp::Await(kValue));
    Value mappedValue = kValue;
    if (mapping) {
      mappedValue = TRY(Call(state->mapfn, state->thisArg, {kValue, Value::number(k)}));
      mappedValue = TRY(interp::Await(mappedValue));
    }
    TRY(CreateDataPropertyOrThrow(A, indexKey(k), mappedValue));
  }
  TRY(Set(A, PropertyKey(u"length"), Value::number(len), true));
  return Value(A);
}

// fromAsyncClosure
Result<Value> fromAsyncBody(interp::SuspendableObject* self) {
  auto* state = static_cast<FromAsyncState*>(self->nativeData);
  bool mapping = false;
  if (!state->mapfn.isUndefined()) {
    if (!IsCallable(state->mapfn)) return ThrowTypeError("Array.fromAsync: mapfn is not a function");
    mapping = true;
  }
  Value items = state->asyncItems;
  Value usingAsyncIterator = TRY(GetMethod(items, PropertyKey(symbols().asyncIterator)));
  Value usingSyncIterator = Value::undefined();
  if (usingAsyncIterator.isUndefined()) usingSyncIterator = TRY(GetMethod(items, PropertyKey(symbols().iterator)));
  if (!usingAsyncIterator.isUndefined()) {
    IteratorRecord record = TRY(GetIteratorFromMethod(items, usingAsyncIterator));
    return fromAsyncIterator(state, record, mapping);
  }
  if (!usingSyncIterator.isUndefined()) {
    IteratorRecord syncRecord = TRY(GetIteratorFromMethod(items, usingSyncIterator));
    IteratorRecord record = TRY(CreateAsyncFromSyncIterator(syncRecord));
    return fromAsyncIterator(state, record, mapping);
  }
  return fromAsyncArrayLike(state, mapping);
}

Result<Value> Array_fromAsync(NativeCall& call) {
  interp::PromiseCapability* promiseCapability =
      interp::NewPromiseCapability(Value(intrinsic(Intrinsic::Promise))).release();
  auto* state = make<FromAsyncState>();
  state->constructor = call.thisValue;
  state->asyncItems = call.args[0];
  state->mapfn = call.args[1];
  state->thisArg = call.args[2];
  interp::InterpContext context;
  context.realm = currentRealm();
  context.function = call.callee;
  interp::AsyncNativeBlockStart(promiseCapability, fromAsyncBody, state, context);
  return promiseCapability->promise;
}

void installArrayFromAsync(Realm& realm) {
  defineFunction(realm, realm.intrinsic(Intrinsic::Array), PropertyKey(u"fromAsync"), 1, Array_fromAsync);
}

}  // namespace

SNOWMAN_BUILTIN(ArrayFromAsync, kOrderLate, installArrayFromAsync);

}  // namespace snowman::builtins
