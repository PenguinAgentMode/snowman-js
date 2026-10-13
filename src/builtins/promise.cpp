// Promise (27.2): the constructor, its statics and combinators, the prototype, and the jobs.
#include "interp/promise.h"

#include "interp/interpreter.h"
#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/symbol.h"
#include "runtime/vm.h"

namespace snowman::interp {

void PromiseCapability::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(promise);
  tracer.visit(resolve);
  tracer.visit(reject);
}

void PromiseReaction::trace(Tracer& tracer) {
  tracer.visit(capability);
  tracer.visit(handler);
}

void PromiseObject::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(result);
  for (PromiseReaction* reaction : fulfillReactions) tracer.visit(reaction);
  for (PromiseReaction* reaction : rejectReactions) tracer.visit(reaction);
}

Result<void> NativeJob::run() {
  if (!realm) return fn(*this);
  ExecutionContext context;
  context.realm = realm;
  ContextScope scope(&context);
  return fn(*this);
}

void NativeJob::trace(Tracer& tracer) {
  Job::trace(tracer);
  tracer.visitAll(values);
}

bool IsPromise(Value value) { return value.isObject() && value.asObject()->is<PromiseObject>(); }

namespace {

Realm* currentRealm() { return vm().currentRealm(); }

// Shared mutable state of a set of built-in closures (an object, never exposed, so slots can hold it).
class Record final : public Object {
 public:
  Record() : Object(nullptr) {}
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visitAll(values);
    for (const PropertyKey& key : keys) key.trace(tracer);
    tracer.visit(capability);
  }
  bool flag = false;
  bool keyed = false;  // Promise.allKeyed / allSettledKeyed: values[i] belongs to keys[i]
  double count = 0;
  std::vector<Value> values;
  std::vector<PropertyKey> keys;
  PromiseCapability* capability = nullptr;
};

BuiltinFunction* closure(NativeFn fn, int length, std::initializer_list<Value> slots) {
  BuiltinFunction* function = createBuiltinFunction(*currentRealm(), fn, length, PropertyKey(emptyString()));
  function->slots.assign(slots.begin(), slots.end());
  return function;
}

// ---- Jobs ----

// NewPromiseReactionJob
class ReactionJob final : public Job {
 public:
  ReactionJob(Realm* realm, PromiseReaction* reaction, Value argument)
      : Job(realm), reaction(reaction), argument(argument) {}

  Result<void> run() override {
    if (!realm) return runInContext();
    ExecutionContext context;
    context.realm = realm;
    ContextScope scope(&context);
    return runInContext();
  }

  Result<void> runInContext() {
    PromiseCapability* promiseCapability = reaction->capability;
    Result<Value> handlerResult = Value::undefined();
    if (reaction->handler.isEmpty()) {
      if (reaction->type == PromiseReaction::Type::Fulfill) {
        handlerResult = argument;
      } else {
        handlerResult = Throw{argument};
      }
    } else {
      handlerResult = Call(reaction->handler, Value::undefined(), {argument});
    }
    if (!promiseCapability) {
      if (handlerResult.isThrow()) return Throw{handlerResult.thrown()};
      return {};
    }
    if (handlerResult.isThrow()) {
      TRY(Call(promiseCapability->reject, Value::undefined(), {handlerResult.thrown()}));
    } else {
      TRY(Call(promiseCapability->resolve, Value::undefined(), {handlerResult.value()}));
    }
    return {};
  }

  void trace(Tracer& tracer) override {
    Job::trace(tracer);
    tracer.visit(reaction);
    tracer.visit(argument);
  }

  PromiseReaction* reaction;
  Value argument;
};

Realm* handlerRealm(Value handler) {
  if (handler.isEmpty()) return nullptr;
  Result<Realm*> realm = GetFunctionRealm(handler.asObject());
  return realm.isNormal() ? realm.value() : currentRealm();
}

void enqueueReactionJob(PromiseReaction* reaction, Value argument) {
  vm().enqueueJob(make<ReactionJob>(handlerRealm(reaction->handler), reaction, argument));
}

void TriggerPromiseReactions(const std::vector<PromiseReaction*>& reactions, Value argument) {
  for (PromiseReaction* reaction : reactions) enqueueReactionJob(reaction, argument);
}

Result<Value> promiseResolveFunction(NativeCall& call);
Result<Value> promiseRejectFunction(NativeCall& call);

struct ResolvingFunctions {
  BuiltinFunction* resolve;
  BuiltinFunction* reject;
};

// CreateResolvingFunctions(promise)
ResolvingFunctions CreateResolvingFunctions(PromiseObject* promise) {
  auto* alreadyResolved = make<Record>();
  BuiltinFunction* resolve = closure(promiseResolveFunction, 1, {Value(promise), Value(alreadyResolved)});
  BuiltinFunction* reject = closure(promiseRejectFunction, 1, {Value(promise), Value(alreadyResolved)});
  return {resolve, reject};
}

// NewPromiseResolveThenableJob
class ResolveThenableJob final : public Job {
 public:
  ResolveThenableJob(Realm* realm, PromiseObject* promise, Value thenable, Value then)
      : Job(realm), promise(promise), thenable(thenable), then(then) {}

  Result<void> run() override {
    ExecutionContext context;
    context.realm = realm;
    ContextScope scope(&context);
    ResolvingFunctions resolvingFunctions = CreateResolvingFunctions(promise);
    Result<Value> thenCallResult =
        Call(then, thenable, {Value(resolvingFunctions.resolve), Value(resolvingFunctions.reject)});
    if (thenCallResult.isThrow()) {
      TRY(Call(Value(resolvingFunctions.reject), Value::undefined(), {thenCallResult.thrown()}));
    }
    return {};
  }

  void trace(Tracer& tracer) override {
    Job::trace(tracer);
    tracer.visit(promise);
    tracer.visit(thenable);
    tracer.visit(then);
  }

  PromiseObject* promise;
  Value thenable;
  Value then;
};

// 27.2.1.3.2 Promise Resolve Functions
Result<Value> promiseResolveFunction(NativeCall& call) {
  auto* promise = call.callee->slots[0].asObject()->as<PromiseObject>();
  auto* alreadyResolved = call.callee->slots[1].asObject()->as<Record>();
  Value resolution = call.args[0];
  if (alreadyResolved->flag) return Value::undefined();
  alreadyResolved->flag = true;
  if (resolution.isObject() && resolution.asObject() == promise) {
    RejectPromise(promise, Value(CreateError(currentRealm(), Intrinsic::TypeErrorPrototype, "Chaining cycle detected for promise")));
    return Value::undefined();
  }
  if (!resolution.isObject()) {
    FulfillPromise(promise, resolution);
    return Value::undefined();
  }
  Result<Value> then = Get(resolution.asObject(), PropertyKey(u"then"));
  if (then.isThrow()) {
    RejectPromise(promise, then.thrown());
    return Value::undefined();
  }
  Value thenAction = then.value();
  if (!IsCallable(thenAction)) {
    FulfillPromise(promise, resolution);
    return Value::undefined();
  }
  Realm* jobRealm = handlerRealm(thenAction);
  vm().enqueueJob(make<ResolveThenableJob>(jobRealm, promise, resolution, thenAction));
  return Value::undefined();
}

// 27.2.1.3.1 Promise Reject Functions
Result<Value> promiseRejectFunction(NativeCall& call) {
  auto* promise = call.callee->slots[0].asObject()->as<PromiseObject>();
  auto* alreadyResolved = call.callee->slots[1].asObject()->as<Record>();
  if (alreadyResolved->flag) return Value::undefined();
  alreadyResolved->flag = true;
  RejectPromise(promise, call.args[0]);
  return Value::undefined();
}

// GetCapabilitiesExecutor Functions
Result<Value> capabilityExecutor(NativeCall& call) {
  auto* capability = call.callee->slots[0].asObject()->as<PromiseCapability>();
  if (!capability->resolve.isUndefined()) return ThrowTypeError("Promise executor has already been invoked");
  if (!capability->reject.isUndefined()) return ThrowTypeError("Promise executor has already been invoked");
  capability->resolve = call.args[0];
  capability->reject = call.args[1];
  return Value::undefined();
}

}  // namespace

void FulfillPromise(PromiseObject* promise, Value value) {
  std::vector<PromiseReaction*> reactions = std::move(promise->fulfillReactions);
  promise->result = value;
  promise->fulfillReactions.clear();
  promise->rejectReactions.clear();
  promise->state = PromiseObject::State::Fulfilled;
  TriggerPromiseReactions(reactions, value);
}

void RejectPromise(PromiseObject* promise, Value reason) {
  std::vector<PromiseReaction*> reactions = std::move(promise->rejectReactions);
  promise->result = reason;
  promise->fulfillReactions.clear();
  promise->rejectReactions.clear();
  promise->state = PromiseObject::State::Rejected;
  // HostPromiseRejectionTracker(promise, "reject"): nothing to track.
  TriggerPromiseReactions(reactions, reason);
}

// 27.2.1.5 NewPromiseCapability
Result<PromiseCapability*> NewPromiseCapability(Value C) {
  if (!IsConstructor(C)) return ThrowTypeError("Promise capability constructor is not a constructor");
  auto* capability = make<PromiseCapability>();
  capability->resolve = Value::undefined();
  capability->reject = Value::undefined();
  BuiltinFunction* executor = closure(capabilityExecutor, 2, {Value(capability)});
  Object* promise = TRY(Construct(C.asObject(), {Value(executor)}));
  if (!IsCallable(capability->resolve)) return ThrowTypeError("Promise resolve function is not callable");
  if (!IsCallable(capability->reject)) return ThrowTypeError("Promise reject function is not callable");
  capability->promise = Value(promise);
  return capability;
}

// 27.2.4.7.1 PromiseResolve
Result<Value> PromiseResolve(Object* C, Value x) {
  if (IsPromise(x)) {
    Value xConstructor = TRY(Get(x.asObject(), PropertyKey(u"constructor")));
    if (xConstructor.isObject() && xConstructor.asObject() == C) return x;
  }
  PromiseCapability* promiseCapability = TRY(NewPromiseCapability(Value(C)));
  TRY(Call(promiseCapability->resolve, Value::undefined(), {x}));
  return promiseCapability->promise;
}

// 27.2.5.4.1 PerformPromiseThen
Value PerformPromiseThen(PromiseObject* promise, Value onFulfilled, Value onRejected,
                         PromiseCapability* resultCapability) {
  Value onFulfilledJobCallback = IsCallable(onFulfilled) ? onFulfilled : Value::empty();
  Value onRejectedJobCallback = IsCallable(onRejected) ? onRejected : Value::empty();
  auto* fulfillReaction = make<PromiseReaction>(resultCapability, PromiseReaction::Type::Fulfill, onFulfilledJobCallback);
  auto* rejectReaction = make<PromiseReaction>(resultCapability, PromiseReaction::Type::Reject, onRejectedJobCallback);
  switch (promise->state) {
    case PromiseObject::State::Pending:
      promise->fulfillReactions.push_back(fulfillReaction);
      promise->rejectReactions.push_back(rejectReaction);
      break;
    case PromiseObject::State::Fulfilled:
      enqueueReactionJob(fulfillReaction, promise->result);
      break;
    case PromiseObject::State::Rejected:
      enqueueReactionJob(rejectReaction, promise->result);
      break;
  }
  promise->isHandled = true;
  if (!resultCapability) return Value::undefined();
  return resultCapability->promise;
}

Result<void> resolveCapability(PromiseCapability* capability, Value value) {
  TRY(Call(capability->resolve, Value::undefined(), {value}));
  return {};
}

Result<void> rejectCapability(PromiseCapability* capability, Value reason) {
  TRY(Call(capability->reject, Value::undefined(), {reason}));
  return {};
}

namespace {

// IfAbruptRejectPromise
template <class T>
bool isAbruptThenReject(const Result<T>& result, PromiseCapability* capability, Result<Value>& out) {
  if (result.isNormal()) return false;
  Result<Value> rejected = Call(capability->reject, Value::undefined(), {result.thrown()});
  if (rejected.isThrow()) {
    out = Throw{rejected.thrown()};
  } else {
    out = capability->promise;
  }
  return true;
}

#define IF_ABRUPT_REJECT_PROMISE(result, capability)                  \
  do {                                                                \
    Result<Value> _abrupt = Value::undefined();                       \
    if (isAbruptThenReject((result), (capability), _abrupt)) return _abrupt; \
  } while (0)

// ---- The constructor and statics ----

// 27.2.3.1 Promise(executor)
Result<Value> promiseConstructor(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Promise constructor cannot be invoked without 'new'");
  Value executor = call.args[0];
  if (!IsCallable(executor)) return ThrowTypeError("Promise resolver is not a function");
  PromiseObject* promise = TRY(OrdinaryCreateFromConstructor<PromiseObject>(call.newTarget, Intrinsic::PromisePrototype));
  ResolvingFunctions resolvingFunctions = CreateResolvingFunctions(promise);
  Result<Value> completion =
      Call(executor, Value::undefined(), {Value(resolvingFunctions.resolve), Value(resolvingFunctions.reject)});
  if (completion.isThrow()) {
    TRY(Call(Value(resolvingFunctions.reject), Value::undefined(), {completion.thrown()}));
  }
  return Value(promise);
}

// GetPromiseResolve
Result<Value> GetPromiseResolve(Value promiseConstructor) {
  Value promiseResolve = TRY(Get(promiseConstructor.asObject(), PropertyKey(u"resolve")));
  if (!IsCallable(promiseResolve)) return ThrowTypeError("Promise resolve is not a function");
  return promiseResolve;
}

enum class Combinator { All, AllSettled, Any };

Value settledResult(const char16_t* status, const char16_t* key, Value value) {
  Object* obj = OrdinaryObjectCreate(currentRealm()->intrinsic(Intrinsic::ObjectPrototype));
  (void)CreateDataPropertyOrThrow(obj, PropertyKey(u"status"), Value(atom(status)));
  (void)CreateDataPropertyOrThrow(obj, PropertyKey(key), value);
  return Value(obj);
}

Value aggregateError(const std::vector<Value>& errors) {
  Object* error = make<ErrorObject>(currentRealm()->intrinsic(Intrinsic::AggregateErrorPrototype));
  Object* list = CreateArrayFromList(Args(errors.data(), errors.size()));
  (void)DefinePropertyOrThrow(error, PropertyKey(u"errors"), PropertyDescriptor::data(Value(list), Attr::WritableConfigurable));
  return Value(error);
}

// CreateKeyedPromiseCombinatorResultObject (await-dictionary)
Value keyedResultObject(Record* record) {
  Object* obj = OrdinaryObjectCreate(nullptr);
  for (size_t i = 0; i < record->keys.size(); i++) {
    (void)CreateDataPropertyOrThrow(obj, record->keys[i], record->values[i]);
  }
  return Value(obj);
}

Value combinatorResult(Record* record) {
  if (record->keyed) return keyedResultObject(record);
  return Value(CreateArrayFromList(Args(record->values.data(), record->values.size())));
}

// slots: [record, index, alreadyCalled record, kind]
Result<Value> combinatorElementFunction(NativeCall& call) {
  auto* record = call.callee->slots[0].asObject()->as<Record>();
  auto index = size_t(call.callee->slots[1].asNumber());
  auto* alreadyCalled = call.callee->slots[2].asObject()->as<Record>();
  int kind = int(call.callee->slots[3].asNumber());  // 0 all, 1 allSettled fulfilled, 2 allSettled rejected, 3 any
  if (alreadyCalled->flag) return Value::undefined();
  alreadyCalled->flag = true;
  Value x = call.args[0];
  switch (kind) {
    case 1:
      record->values[index] = settledResult(u"fulfilled", u"value", x);
      break;
    case 2:
      record->values[index] = settledResult(u"rejected", u"reason", x);
      break;
    default:
      record->values[index] = x;
      break;
  }
  record->count--;
  if (record->count == 0) {
    if (kind == 3) {
      TRY(Call(record->capability->reject, Value::undefined(), {aggregateError(record->values)}));
      return Value::undefined();
    }
    return Call(record->capability->resolve, Value::undefined(), {combinatorResult(record)});
  }
  return Value::undefined();
}

// PerformPromiseAll / PerformPromiseAllSettled / PerformPromiseAny
Result<Value> performCombinator(Combinator kind, IteratorRecord& iteratorRecord, Value constructor,
                                PromiseCapability* resultCapability, Value promiseResolve) {
  auto* record = make<Record>();
  record->capability = resultCapability;
  record->count = 1;
  uint32_t index = 0;
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(iteratorRecord));
    if (!next) {
      record->count--;
      if (record->count == 0) {
        if (kind == Combinator::Any) return Throw{aggregateError(record->values)};
        Object* valuesArray = CreateArrayFromList(Args(record->values.data(), record->values.size()));
        TRY(Call(resultCapability->resolve, Value::undefined(), {Value(valuesArray)}));
      }
      return resultCapability->promise;
    }
    record->values.push_back(Value::undefined());
    Value nextPromise = TRY(Call(promiseResolve, constructor, {*next}));
    auto* alreadyCalled = make<Record>();
    Value onFulfilled;
    Value onRejected;
    switch (kind) {
      case Combinator::All:
        onFulfilled = Value(closure(combinatorElementFunction, 1,
                                    {Value(record), Value::number(index), Value(alreadyCalled), Value::number(0)}));
        onRejected = resultCapability->reject;
        break;
      case Combinator::AllSettled:
        onFulfilled = Value(closure(combinatorElementFunction, 1,
                                    {Value(record), Value::number(index), Value(alreadyCalled), Value::number(1)}));
        onRejected = Value(closure(combinatorElementFunction, 1,
                                   {Value(record), Value::number(index), Value(alreadyCalled), Value::number(2)}));
        break;
      case Combinator::Any:
        onFulfilled = resultCapability->resolve;
        onRejected = Value(closure(combinatorElementFunction, 1,
                                   {Value(record), Value::number(index), Value(alreadyCalled), Value::number(3)}));
        break;
    }
    record->count++;
    TRY(Invoke(nextPromise, PropertyKey(u"then"), {onFulfilled, onRejected}));
    index++;
  }
}

Result<Value> combinator(NativeCall& call, Combinator kind) {
  Value C = call.thisValue;
  PromiseCapability* promiseCapability = TRY(NewPromiseCapability(C));
  Result<Value> promiseResolve = GetPromiseResolve(C);
  IF_ABRUPT_REJECT_PROMISE(promiseResolve, promiseCapability);
  Result<IteratorRecord> iteratorRecord = GetIterator(call.args[0], IteratorKind::Sync);
  IF_ABRUPT_REJECT_PROMISE(iteratorRecord, promiseCapability);
  Result<Value> result =
      performCombinator(kind, iteratorRecord.value(), C, promiseCapability, promiseResolve.value());
  if (result.isThrow()) {
    if (!iteratorRecord.value().done) {
      Result<void> closed = IteratorClose(iteratorRecord.value(), Throw{result.thrown()});
      result = Throw{closed.thrown()};
    }
    IF_ABRUPT_REJECT_PROMISE(result, promiseCapability);
  }
  return result;
}

Result<Value> promiseAll(NativeCall& call) { return combinator(call, Combinator::All); }
Result<Value> promiseAllSettled(NativeCall& call) { return combinator(call, Combinator::AllSettled); }
Result<Value> promiseAny(NativeCall& call) { return combinator(call, Combinator::Any); }

// PerformPromiseAllKeyed (await-dictionary): variant is All or AllSettled.
Result<Value> performPromiseAllKeyed(Combinator variant, Object* promises, Value constructor,
                                     PromiseCapability* resultCapability, Value promiseResolve) {
  std::vector<PropertyKey> allKeys = TRY(promises->OwnPropertyKeys());
  RootedKeys rootedKeys(allKeys);
  auto* record = make<Record>();
  record->keyed = true;
  record->capability = resultCapability;
  record->count = 1;
  uint32_t index = 0;
  for (const PropertyKey& key : allKeys) {
    std::optional<PropertyDescriptor> desc = TRY(promises->GetOwnProperty(key));
    if (!desc || !desc->enumerable.value_or(false)) continue;
    Value value = TRY(Get(promises, key));
    record->keys.push_back(key);
    record->values.push_back(Value::undefined());
    Value nextPromise = TRY(Call(promiseResolve, constructor, {value}));
    auto* alreadyCalled = make<Record>();
    Value onFulfilled = Value(closure(combinatorElementFunction, 1,
                                      {Value(record), Value::number(index), Value(alreadyCalled),
                                       Value::number(variant == Combinator::All ? 0 : 1)}));
    Value onRejected = resultCapability->reject;
    if (variant == Combinator::AllSettled) {
      onRejected = Value(closure(combinatorElementFunction, 1,
                                 {Value(record), Value::number(index), Value(alreadyCalled), Value::number(2)}));
    }
    record->count++;
    TRY(Invoke(nextPromise, PropertyKey(u"then"), {onFulfilled, onRejected}));
    index++;
  }
  record->count--;
  if (record->count == 0) {
    TRY(Call(resultCapability->resolve, Value::undefined(), {keyedResultObject(record)}));
  }
  return resultCapability->promise;
}

// Promise.allKeyed / Promise.allSettledKeyed (await-dictionary)
Result<Value> keyedCombinator(NativeCall& call, Combinator variant) {
  Value C = call.thisValue;
  PromiseCapability* promiseCapability = TRY(NewPromiseCapability(C));
  Result<Value> promiseResolve = GetPromiseResolve(C);
  IF_ABRUPT_REJECT_PROMISE(promiseResolve, promiseCapability);
  Value promises = call.args[0];
  if (!promises.isObject()) {
    Value error = Value(CreateError(currentRealm(), Intrinsic::TypeErrorPrototype, "argument is not an object"));
    TRY(Call(promiseCapability->reject, Value::undefined(), {error}));
    return promiseCapability->promise;
  }
  Result<Value> result =
      performPromiseAllKeyed(variant, promises.asObject(), C, promiseCapability, promiseResolve.value());
  IF_ABRUPT_REJECT_PROMISE(result, promiseCapability);
  return result;
}

Result<Value> promiseAllKeyed(NativeCall& call) { return keyedCombinator(call, Combinator::All); }
Result<Value> promiseAllSettledKeyed(NativeCall& call) { return keyedCombinator(call, Combinator::AllSettled); }

// 27.2.4.5 Promise.race
Result<Value> performPromiseRace(IteratorRecord& iteratorRecord, Value constructor,
                                 PromiseCapability* resultCapability, Value promiseResolve) {
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(iteratorRecord));
    if (!next) return resultCapability->promise;
    Value nextPromise = TRY(Call(promiseResolve, constructor, {*next}));
    TRY(Invoke(nextPromise, PropertyKey(u"then"), {resultCapability->resolve, resultCapability->reject}));
  }
}

Result<Value> promiseRace(NativeCall& call) {
  Value C = call.thisValue;
  PromiseCapability* promiseCapability = TRY(NewPromiseCapability(C));
  Result<Value> promiseResolve = GetPromiseResolve(C);
  IF_ABRUPT_REJECT_PROMISE(promiseResolve, promiseCapability);
  Result<IteratorRecord> iteratorRecord = GetIterator(call.args[0], IteratorKind::Sync);
  IF_ABRUPT_REJECT_PROMISE(iteratorRecord, promiseCapability);
  Result<Value> result = performPromiseRace(iteratorRecord.value(), C, promiseCapability, promiseResolve.value());
  if (result.isThrow()) {
    if (!iteratorRecord.value().done) {
      Result<void> closed = IteratorClose(iteratorRecord.value(), Throw{result.thrown()});
      result = Throw{closed.thrown()};
    }
    IF_ABRUPT_REJECT_PROMISE(result, promiseCapability);
  }
  return result;
}

// 27.2.4.6 Promise.reject
Result<Value> promiseReject(NativeCall& call) {
  PromiseCapability* promiseCapability = TRY(NewPromiseCapability(call.thisValue));
  TRY(Call(promiseCapability->reject, Value::undefined(), {call.args[0]}));
  return promiseCapability->promise;
}

// 27.2.4.7 Promise.resolve
Result<Value> promiseResolveStatic(NativeCall& call) {
  if (!call.thisValue.isObject()) return ThrowTypeError("Promise.resolve called on a non-object");
  return PromiseResolve(call.thisValue.asObject(), call.args[0]);
}

// 27.2.4.8 Promise.try
// A normal completion goes through PromiseResolve, so a promise of C is returned as it is (not wrapped).
Result<Value> promiseTry(NativeCall& call) {
  if (!call.thisValue.isObject()) return ThrowTypeError("Promise.try called on a non-object");
  Result<Value> status = Call(call.args[0], Value::undefined(), call.args.slice(1));
  if (status.isNormal()) return PromiseResolve(call.thisValue.asObject(), status.value());
  PromiseCapability* promiseCapability = TRY(NewPromiseCapability(call.thisValue));
  TRY(Call(promiseCapability->reject, Value::undefined(), {status.thrown()}));
  return promiseCapability->promise;
}

// 27.2.4.9 Promise.withResolvers
Result<Value> promiseWithResolvers(NativeCall& call) {
  PromiseCapability* promiseCapability = TRY(NewPromiseCapability(call.thisValue));
  Object* obj = OrdinaryObjectCreate(currentRealm()->intrinsic(Intrinsic::ObjectPrototype));
  TRY(CreateDataPropertyOrThrow(obj, PropertyKey(u"promise"), promiseCapability->promise));
  TRY(CreateDataPropertyOrThrow(obj, PropertyKey(u"resolve"), promiseCapability->resolve));
  TRY(CreateDataPropertyOrThrow(obj, PropertyKey(u"reject"), promiseCapability->reject));
  return Value(obj);
}

Result<Value> promiseSpecies(NativeCall& call) { return call.thisValue; }

// ---- The prototype ----

// 27.2.5.4 Promise.prototype.then
Result<Value> promiseThen(NativeCall& call) {
  Value promise = call.thisValue;
  if (!IsPromise(promise)) return ThrowTypeError("Promise.prototype.then called on a non-promise");
  Object* C = TRY(SpeciesConstructor(promise.asObject(), currentRealm()->intrinsic(Intrinsic::Promise)));
  PromiseCapability* resultCapability = TRY(NewPromiseCapability(Value(C)));
  return PerformPromiseThen(promise.asObject()->as<PromiseObject>(), call.args[0], call.args[1], resultCapability);
}

// 27.2.5.1 Promise.prototype.catch
Result<Value> promiseCatch(NativeCall& call) {
  return Invoke(call.thisValue, PropertyKey(u"then"), {Value::undefined(), call.args[0]});
}

// slots: [value]
Result<Value> valueThunk(NativeCall& call) { return call.callee->slots[0]; }
Result<Value> thrower(NativeCall& call) { return Throw{call.callee->slots[0]}; }

// slots: [onFinally, C]; thenFinally (fulfilled) and catchFinally (rejected)
Result<Value> finallyReaction(NativeCall& call, bool rejected) {
  Value onFinally = call.callee->slots[0];
  Object* C = call.callee->slots[1].asObject();
  Value result = TRY(Call(onFinally, Value::undefined()));
  Value p = TRY(PromiseResolve(C, result));
  BuiltinFunction* next = closure(rejected ? thrower : valueThunk, 0, {call.args[0]});
  return Invoke(p, PropertyKey(u"then"), {Value(next)});
}
Result<Value> thenFinally(NativeCall& call) { return finallyReaction(call, false); }
Result<Value> catchFinally(NativeCall& call) { return finallyReaction(call, true); }

// 27.2.5.3 Promise.prototype.finally
Result<Value> promiseFinally(NativeCall& call) {
  Value promise = call.thisValue;
  if (!promise.isObject()) return ThrowTypeError("Promise.prototype.finally called on a non-object");
  Object* C = TRY(SpeciesConstructor(promise.asObject(), currentRealm()->intrinsic(Intrinsic::Promise)));
  Value onFinally = call.args[0];
  Value thenFinallyFn = onFinally;
  Value catchFinallyFn = onFinally;
  if (IsCallable(onFinally)) {
    thenFinallyFn = Value(closure(thenFinally, 1, {onFinally, Value(C)}));
    catchFinallyFn = Value(closure(catchFinally, 1, {onFinally, Value(C)}));
  }
  return Invoke(promise, PropertyKey(u"then"), {thenFinallyFn, catchFinallyFn});
}

void installPromise(Realm& realm) {
  Object* prototype = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* constructor = defineConstructor(realm, Intrinsic::Promise, promiseConstructor, 1,
                                                   PropertyKey(u"Promise"), prototype, Intrinsic::PromisePrototype);
  defineFunction(realm, constructor, PropertyKey(u"all"), 1, promiseAll);
  defineFunction(realm, constructor, PropertyKey(u"allSettled"), 1, promiseAllSettled);
  defineFunction(realm, constructor, PropertyKey(u"allKeyed"), 1, promiseAllKeyed);
  defineFunction(realm, constructor, PropertyKey(u"allSettledKeyed"), 1, promiseAllSettledKeyed);
  defineFunction(realm, constructor, PropertyKey(u"any"), 1, promiseAny);
  defineFunction(realm, constructor, PropertyKey(u"race"), 1, promiseRace);
  defineFunction(realm, constructor, PropertyKey(u"reject"), 1, promiseReject);
  defineFunction(realm, constructor, PropertyKey(u"resolve"), 1, promiseResolveStatic);
  defineFunction(realm, constructor, PropertyKey(u"try"), 1, promiseTry);
  defineFunction(realm, constructor, PropertyKey(u"withResolvers"), 0, promiseWithResolvers);
  defineAccessor(realm, constructor, PropertyKey(vm().symbols.species), promiseSpecies, nullptr);
  defineFunction(realm, prototype, PropertyKey(u"catch"), 1, promiseCatch);
  defineFunction(realm, prototype, PropertyKey(u"finally"), 1, promiseFinally);
  defineFunction(realm, prototype, PropertyKey(u"then"), 2, promiseThen);
  defineValue(prototype, PropertyKey(vm().symbols.toStringTag), Value(atom(u"Promise")), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(Promise, kOrderDefault, installPromise);

}  // namespace snowman::interp
