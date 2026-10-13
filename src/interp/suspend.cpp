// Generators (27.5), async functions (27.7), async generators (27.6), Yield / Await / yield* (15.5) and
// AsyncIteratorClose (7.4.13). Every body runs on its own coroutine (coroutine.h); the resumer pushes the
// body's execution context before switching in and restores its own after switching back.
#include "interp/suspend.h"

#include "interp/dispose.h"

#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/vm.h"

namespace snowman::interp {

namespace {

void copyContext(InterpContext& to, const InterpContext& from) {
  to.realm = from.realm;
  to.function = from.function;
  to.lexicalEnvironment = from.lexicalEnvironment;
  to.variableEnvironment = from.variableEnvironment;
  to.privateEnvironment = from.privateEnvironment;
  to.scriptOrModule = from.scriptOrModule;
  to.strict = from.strict;
}

void traceContext(Tracer& tracer, InterpContext& context) {
  tracer.visit(reinterpret_cast<Cell*>(context.realm));
  tracer.visit(context.function);
  tracer.visit(context.lexicalEnvironment);
  tracer.visit(context.variableEnvironment);
  tracer.visit(context.privateEnvironment);
  tracer.visit(context.scriptOrModule);
}

void bodyEntry(Coroutine*, void* data) {
  auto* self = static_cast<SuspendableObject*>(data);
  Result<Value> result = self->function ? evaluateGeneratorLikeBody(self->function) : self->nativeBody(self);
  self->finished = true;
  if (result.isThrow()) {
    self->outcomeIsThrow = true;
    self->outcome = result.thrown();
  } else {
    self->outcome = result.value();
  }
}

// The completion a suspended body was resumed with.
struct Resumption {
  ResumeKind kind;
  Value value;
};

Resumption resumptionOf(SuspendableObject* owner) { return {owner->suspend.resumeKind, owner->suspend.resumeValue}; }

// A resumption as the result of the suspending expression (a return travels as the return marker).
Result<Value> toResult(SuspendableObject* owner, Resumption resumption) {
  switch (resumption.kind) {
    case ResumeKind::Normal:
      return resumption.value;
    case ResumeKind::Throw:
      return Throw{resumption.value};
    case ResumeKind::Return:
      owner->suspend.returnValue = resumption.value;
      return returnMarker();
  }
  return resumption.value;
}

SuspendableObject* runningOwner() {
  InterpContext* context = runningContext();
  return context ? static_cast<SuspendableObject*>(context->suspendOwner) : nullptr;
}

Realm* currentRealm() { return vm().currentRealm(); }

BuiltinFunction* closure(NativeFn fn, int length, Value slot) {
  BuiltinFunction* function = createBuiltinFunction(*currentRealm(), fn, length, PropertyKey(emptyString()));
  function->slots.push_back(slot);
  return function;
}

void afterBodyStep(SuspendableObject* owner);

}  // namespace

void SuspendableObject::trace(Tracer& tracer) {
  Object::trace(tracer);
  traceContext(tracer, context);
  tracer.visit(suspend.resumeValue);
  tracer.visit(suspend.returnValue);
  tracer.visit(function);
  tracer.visit(nativeData);
  tracer.visit(outcome);
}

void GeneratorObject::trace(Tracer& tracer) {
  SuspendableObject::trace(tracer);
  tracer.visit(brand);
}

void AsyncGeneratorObject::trace(Tracer& tracer) {
  SuspendableObject::trace(tracer);
  for (auto& request : queue) {
    tracer.visit(request.value);
    tracer.visit(request.capability);
  }
}

void AsyncState::trace(Tracer& tracer) {
  SuspendableObject::trace(tracer);
  tracer.visit(capability);
}

void SuspendableObject::resumeBody(ResumeKind resumeKind, Value value) {
  suspend.resumeKind = resumeKind;
  suspend.resumeValue = value;
  if (!coroutine) {
    coroutine = std::make_unique<Coroutine>(bodyEntry, this);
    suspend.coroutine = coroutine.get();
    context.suspendOwner = this;
    context.suspend = &suspend;
  }
  ExecutionContext* saved = vm().running;
  vm().pushContext(&context);
  coroutine->resume();
  vm().running = saved;
}

void SuspendableObject::suspendBody() { coroutine->suspend(); }

bool inAsyncGenerator() {
  SuspendableObject* owner = runningOwner();
  return owner && owner->kind == SuspendableObject::Kind::AsyncGenerator;
}

Result<Value> evaluateGeneratorLikeBody(ECMAScriptFunction* F) {
  const ast::Function* function = F->function;
  Result<Value> result = Value::undefined();
  if (function->expression) {
    result = evaluate(function->body);
  } else {
    Environment* lexEnv = runningContext()->lexicalEnvironment;
    Result<Completion> completion =
        DisposeResources(lexEnv, evaluateStatementList(function->body->as<ast::BlockStatement>()->body));
    if (completion.isThrow()) {
      result = Throw{completion.thrown()};
    } else if (completion.value().type == CompletionType::Return) {
      result = completion.value().value;
    }
  }
  if (result.isThrow() && isReturnMarker(result.thrown())) return runningContext()->suspend->returnValue;
  return result;
}

// ---- Await (6.2.3.1) ----

namespace {

Result<Value> awaitFulfilled(NativeCall& call) {
  auto* owner = static_cast<SuspendableObject*>(call.callee->slots[0].asObject());
  owner->resumeBody(ResumeKind::Normal, call.args[0]);
  afterBodyStep(owner);
  return Value::undefined();
}

Result<Value> awaitRejected(NativeCall& call) {
  auto* owner = static_cast<SuspendableObject*>(call.callee->slots[0].asObject());
  owner->resumeBody(ResumeKind::Throw, call.args[0]);
  afterBodyStep(owner);
  return Value::undefined();
}

}  // namespace

Result<Value> Await(Value value) {
  SuspendableObject* owner = runningOwner();
  if (!owner) return ThrowSyntaxError("await is only valid in async functions and the top level of modules");
  Value promise = TRY(PromiseResolve(currentRealm()->intrinsic(Intrinsic::Promise), value));
  BuiltinFunction* onFulfilled = closure(awaitFulfilled, 1, Value(owner));
  BuiltinFunction* onRejected = closure(awaitRejected, 1, Value(owner));
  PerformPromiseThen(promise.asObject()->as<PromiseObject>(), Value(onFulfilled), Value(onRejected));
  owner->suspendBody();
  return toResult(owner, resumptionOf(owner));
}

// ---- Generators ----

Result<Value> startGeneratorBody(ECMAScriptFunction* F, InterpContext* context) {
  GeneratorObject* G = TRY(OrdinaryCreateFromConstructor<GeneratorObject>(F, Intrinsic::GeneratorPrototype));
  copyContext(G->context, *context);
  G->function = F;
  G->state = GeneratorObject::State::SuspendedStart;
  return Value(G);
}

namespace {

// GeneratorValidate
Result<GeneratorObject*> GeneratorValidate(Value generator, Value brand) {
  GeneratorObject* G = generator.isObject() ? generator.asObject()->as<GeneratorObject>() : nullptr;
  if (!G) return ThrowTypeError("not a generator object");
  if (!SameValue(G->brand, brand)) return ThrowTypeError("generator brand mismatch");
  if (G->state == GeneratorObject::State::Executing) return ThrowTypeError("Generator is already running");
  return G;
}

// What GeneratorResume returns once the body has suspended or finished.
Result<Value> generatorStepResult(GeneratorObject* G) {
  if (!G->finished) return G->outcome;  // the yielded iterator result
  G->state = GeneratorObject::State::Completed;
  if (G->outcomeIsThrow) return Throw{G->outcome};
  return Value(CreateIteratorResultObject(G->outcome, true));
}

// GeneratorYield(iteratorResult), without interpreting the resumption.
Resumption generatorYield(GeneratorObject* G, Value iteratorResult) {
  G->state = GeneratorObject::State::SuspendedYield;
  G->outcome = iteratorResult;
  G->suspendBody();
  return resumptionOf(G);
}

}  // namespace

Result<Value> GeneratorResume(Value generator, Value value, Value brand) {
  GeneratorObject* G = TRY(GeneratorValidate(generator, brand));
  if (G->state == GeneratorObject::State::Completed) return Value(CreateIteratorResultObject(Value::undefined(), true));
  G->state = GeneratorObject::State::Executing;
  G->resumeBody(ResumeKind::Normal, value);
  return generatorStepResult(G);
}

Result<Value> GeneratorResumeAbrupt(Value generator, ResumeKind kind, Value value, Value brand) {
  GeneratorObject* G = TRY(GeneratorValidate(generator, brand));
  if (G->state == GeneratorObject::State::SuspendedStart) G->state = GeneratorObject::State::Completed;
  if (G->state == GeneratorObject::State::Completed) {
    if (kind == ResumeKind::Return) return Value(CreateIteratorResultObject(value, true));
    return Throw{value};
  }
  G->state = GeneratorObject::State::Executing;
  G->resumeBody(kind, value);
  return generatorStepResult(G);
}

// ---- Async functions ----

Result<Value> startAsyncFunctionBody(ECMAScriptFunction* F, InterpContext* context, Args args) {
  Realm* realm = currentRealm();
  PromiseCapability* promiseCapability = NewPromiseCapability(Value(realm->intrinsic(Intrinsic::Promise))).release();
  Result<void> declResult = FunctionDeclarationInstantiation(F, args);
  if (declResult.isThrow()) {
    (void)rejectCapability(promiseCapability, declResult.thrown());
    return promiseCapability->promise;
  }
  auto* state = snowman::make<AsyncState>(SuspendableObject::Kind::AsyncFunction);
  copyContext(state->context, *context);
  state->function = F;
  state->capability = promiseCapability;
  state->resumeBody(ResumeKind::Normal, Value::undefined());
  afterBodyStep(state);
  return promiseCapability->promise;
}

void AsyncNativeBlockStart(PromiseCapability* capability, SuspendableObject::NativeBody body, Cell* data,
                           InterpContext& context) {
  auto* state = snowman::make<AsyncState>(SuspendableObject::Kind::Module);
  copyContext(state->context, context);
  state->nativeBody = body;
  state->nativeData = data;
  state->capability = capability;
  state->resumeBody(ResumeKind::Normal, Value::undefined());
  afterBodyStep(state);
}

// ---- Async generators ----

Result<Value> startAsyncGeneratorBody(ECMAScriptFunction* F, InterpContext* context) {
  AsyncGeneratorObject* generator =
      TRY(OrdinaryCreateFromConstructor<AsyncGeneratorObject>(F, Intrinsic::AsyncGeneratorPrototype));
  copyContext(generator->context, *context);
  generator->function = F;
  generator->state = AsyncGeneratorObject::State::SuspendedStart;
  return Value(generator);
}

namespace {

using AGState = AsyncGeneratorObject::State;

// AsyncGeneratorCompleteStep(generator, completion, done[, realm])
void AsyncGeneratorCompleteStep(AsyncGeneratorObject* generator, bool isThrow, Value value, bool done,
                                Realm* realm = nullptr) {
  AsyncGeneratorObject::Request next = generator->queue.front();
  generator->queue.pop_front();
  if (isThrow) {
    (void)rejectCapability(next.capability, value);
    return;
  }
  Object* iteratorResult;
  if (realm && vm().running) {
    Realm* oldRealm = vm().running->realm;
    vm().running->realm = realm;
    iteratorResult = CreateIteratorResultObject(value, done);
    vm().running->realm = oldRealm;
  } else {
    iteratorResult = CreateIteratorResultObject(value, done);
  }
  (void)resolveCapability(next.capability, Value(iteratorResult));
}

void AsyncGeneratorDrainQueue(AsyncGeneratorObject* generator);

Result<Value> awaitReturnFulfilled(NativeCall& call) {
  auto* generator = call.callee->slots[0].asObject()->as<AsyncGeneratorObject>();
  AsyncGeneratorCompleteStep(generator, false, call.args[0], true);
  AsyncGeneratorDrainQueue(generator);
  return Value::undefined();
}

Result<Value> awaitReturnRejected(NativeCall& call) {
  auto* generator = call.callee->slots[0].asObject()->as<AsyncGeneratorObject>();
  AsyncGeneratorCompleteStep(generator, true, call.args[0], true);
  AsyncGeneratorDrainQueue(generator);
  return Value::undefined();
}

// AsyncGeneratorAwaitReturn
void AsyncGeneratorAwaitReturn(AsyncGeneratorObject* generator) {
  Value value = generator->queue.front().value;
  Result<Value> promiseCompletion = PromiseResolve(currentRealm()->intrinsic(Intrinsic::Promise), value);
  if (promiseCompletion.isThrow()) {
    AsyncGeneratorCompleteStep(generator, true, promiseCompletion.thrown(), true);
    AsyncGeneratorDrainQueue(generator);
    return;
  }
  BuiltinFunction* onFulfilled = closure(awaitReturnFulfilled, 1, Value(generator));
  BuiltinFunction* onRejected = closure(awaitReturnRejected, 1, Value(generator));
  PerformPromiseThen(promiseCompletion.value().asObject()->as<PromiseObject>(), Value(onFulfilled), Value(onRejected));
}

// AsyncGeneratorDrainQueue
void AsyncGeneratorDrainQueue(AsyncGeneratorObject* generator) {
  if (generator->queue.empty()) {
    generator->state = AGState::Completed;
    return;
  }
  while (true) {
    AsyncGeneratorObject::Request& next = generator->queue.front();
    if (next.kind == ResumeKind::Return) {
      AsyncGeneratorAwaitReturn(generator);
      return;
    }
    bool isThrow = next.kind == ResumeKind::Throw;
    AsyncGeneratorCompleteStep(generator, isThrow, isThrow ? next.value : Value::undefined(), true);
    if (generator->queue.empty()) {
      generator->state = AGState::Completed;
      return;
    }
  }
}

// AsyncGeneratorResume
void AsyncGeneratorResume(AsyncGeneratorObject* generator, ResumeKind kind, Value value) {
  generator->state = AGState::Executing;
  generator->resumeBody(kind, value);
  afterBodyStep(generator);
}

// AsyncGeneratorUnwrapYieldResumption, keeping a return as a resumption.
Resumption unwrapYieldResumption(Resumption resumption) {
  if (resumption.kind != ResumeKind::Return) return resumption;
  Result<Value> awaited = Await(resumption.value);
  if (awaited.isThrow()) return {ResumeKind::Throw, awaited.thrown()};
  return {ResumeKind::Return, awaited.value()};
}

// AsyncGeneratorYield(value), without interpreting a return resumption.
Resumption asyncGeneratorYield(AsyncGeneratorObject* generator, Value value) {
  ExecutionContext* genContext = vm().running;
  Realm* previousRealm = genContext->previous ? genContext->previous->realm : nullptr;
  AsyncGeneratorCompleteStep(generator, false, value, false, previousRealm);
  if (!generator->queue.empty()) {
    const AsyncGeneratorObject::Request& toYield = generator->queue.front();
    return unwrapYieldResumption({toYield.kind, toYield.value});
  }
  generator->state = AGState::SuspendedYield;
  generator->suspendBody();
  return unwrapYieldResumption(resumptionOf(generator));
}

// What happens when a body suspends or finishes, by kind of body (the steps after "Resume ... " in
// AsyncBlockStart, AsyncGeneratorStart).
void afterBodyStep(SuspendableObject* owner) {
  if (!owner->finished) return;
  switch (owner->kind) {
    case SuspendableObject::Kind::AsyncFunction:
    case SuspendableObject::Kind::Module: {
      auto* state = static_cast<AsyncState*>(owner);
      if (owner->outcomeIsThrow) {
        (void)rejectCapability(state->capability, owner->outcome);
      } else {
        (void)resolveCapability(state->capability, owner->outcome);
      }
      break;
    }
    case SuspendableObject::Kind::AsyncGenerator: {
      auto* generator = static_cast<AsyncGeneratorObject*>(owner);
      generator->state = AGState::DrainingQueue;
      AsyncGeneratorCompleteStep(generator, owner->outcomeIsThrow, owner->outcome, true);
      AsyncGeneratorDrainQueue(generator);
      break;
    }
    case SuspendableObject::Kind::Generator:
      break;
  }
}

// AsyncGeneratorValidate + the capability, or the rejected promise to return.
AsyncGeneratorObject* validateAsyncGenerator(Value generator, PromiseCapability* capability) {
  AsyncGeneratorObject* G = generator.isObject() ? generator.asObject()->as<AsyncGeneratorObject>() : nullptr;
  if (!G) {
    Value error(CreateError(currentRealm(), Intrinsic::TypeErrorPrototype, "not an async generator object"));
    (void)rejectCapability(capability, error);
  }
  return G;
}

PromiseCapability* intrinsicCapability() {
  return NewPromiseCapability(Value(currentRealm()->intrinsic(Intrinsic::Promise))).release();
}

}  // namespace

Value AsyncGeneratorNext(Value generatorValue, Value value) {
  PromiseCapability* promiseCapability = intrinsicCapability();
  AsyncGeneratorObject* generator = validateAsyncGenerator(generatorValue, promiseCapability);
  if (!generator) return promiseCapability->promise;
  AGState state = generator->state;
  if (state == AGState::Completed) {
    (void)resolveCapability(promiseCapability, Value(CreateIteratorResultObject(Value::undefined(), true)));
    return promiseCapability->promise;
  }
  generator->queue.push_back({ResumeKind::Normal, value, promiseCapability});
  if (state == AGState::SuspendedStart || state == AGState::SuspendedYield) {
    AsyncGeneratorResume(generator, ResumeKind::Normal, value);
  }
  return promiseCapability->promise;
}

Value AsyncGeneratorReturn(Value generatorValue, Value value) {
  PromiseCapability* promiseCapability = intrinsicCapability();
  AsyncGeneratorObject* generator = validateAsyncGenerator(generatorValue, promiseCapability);
  if (!generator) return promiseCapability->promise;
  generator->queue.push_back({ResumeKind::Return, value, promiseCapability});
  AGState state = generator->state;
  if (state == AGState::SuspendedStart || state == AGState::Completed) {
    generator->state = AGState::DrainingQueue;
    AsyncGeneratorAwaitReturn(generator);
  } else if (state == AGState::SuspendedYield) {
    AsyncGeneratorResume(generator, ResumeKind::Return, value);
  }
  return promiseCapability->promise;
}

Value AsyncGeneratorThrow(Value generatorValue, Value exception) {
  PromiseCapability* promiseCapability = intrinsicCapability();
  AsyncGeneratorObject* generator = validateAsyncGenerator(generatorValue, promiseCapability);
  if (!generator) return promiseCapability->promise;
  AGState state = generator->state;
  if (state == AGState::SuspendedStart) {
    generator->state = AGState::Completed;
    state = AGState::Completed;
  }
  if (state == AGState::Completed) {
    (void)rejectCapability(promiseCapability, exception);
    return promiseCapability->promise;
  }
  generator->queue.push_back({ResumeKind::Throw, exception, promiseCapability});
  if (state == AGState::SuspendedYield) AsyncGeneratorResume(generator, ResumeKind::Throw, exception);
  return promiseCapability->promise;
}

// ---- Yield ----

Result<Value> Yield(Value value) {
  SuspendableObject* owner = runningOwner();
  if (owner->kind == SuspendableObject::Kind::AsyncGenerator) {
    Value awaited = TRY(Await(value));
    return toResult(owner, asyncGeneratorYield(static_cast<AsyncGeneratorObject*>(owner), awaited));
  }
  auto* G = static_cast<GeneratorObject*>(owner);
  return toResult(owner, generatorYield(G, Value(CreateIteratorResultObject(value, false))));
}

Result<Value> evaluateYield(const ast::YieldExpression* node) {
  Value value = Value::undefined();
  if (node->argument) value = TRY(evaluate(node->argument));
  return Yield(value);
}

// 15.5.5 YieldExpression : yield * AssignmentExpression
Result<Value> evaluateYieldStar(const ast::YieldExpression* node) {
  SuspendableObject* owner = runningOwner();
  bool async = owner->kind == SuspendableObject::Kind::AsyncGenerator;
  Value value = TRY(evaluate(node->argument));
  IteratorRecord iteratorRecord = TRY(GetIterator(value, async ? IteratorKind::Async : IteratorKind::Sync));
  Object* iterator = iteratorRecord.iterator;
  Resumption received{ResumeKind::Normal, Value::undefined()};

  // The step after an inner result that is not done: yield it on.
  auto yieldOn = [&](Object* innerResult) -> Result<Resumption> {
    if (async) {
      Value innerValue = TRY(IteratorValue(innerResult));
      return asyncGeneratorYield(static_cast<AsyncGeneratorObject*>(owner), innerValue);
    }
    return generatorYield(static_cast<GeneratorObject*>(owner), Value(innerResult));
  };

  while (true) {
    if (received.kind == ResumeKind::Normal) {
      Value innerResult = TRY(Call(iteratorRecord.nextMethod, Value(iterator), {received.value}));
      if (async) innerResult = TRY(Await(innerResult));
      if (!innerResult.isObject()) return ThrowTypeError("Iterator result is not an object");
      bool done = TRY(IteratorComplete(innerResult.asObject()));
      if (done) return IteratorValue(innerResult.asObject());
      received = TRY(yieldOn(innerResult.asObject()));
    } else if (received.kind == ResumeKind::Throw) {
      Value throwMethod = TRY(GetMethod(Value(iterator), PropertyKey(u"throw")));
      if (!throwMethod.isUndefined()) {
        Value innerResult = TRY(Call(throwMethod, Value(iterator), {received.value}));
        if (async) innerResult = TRY(Await(innerResult));
        if (!innerResult.isObject()) return ThrowTypeError("Iterator result is not an object");
        bool done = TRY(IteratorComplete(innerResult.asObject()));
        if (done) return IteratorValue(innerResult.asObject());
        received = TRY(yieldOn(innerResult.asObject()));
      } else {
        if (async) {
          TRY(AsyncIteratorClose(iteratorRecord, Result<void>()));
        } else {
          TRY(IteratorClose(iteratorRecord, Result<void>()));
        }
        return ThrowTypeError("The iterator does not provide a 'throw' method");
      }
    } else {
      Value returnMethod = TRY(GetMethod(Value(iterator), PropertyKey(u"return")));
      if (returnMethod.isUndefined()) {
        Value returnValue = received.value;
        if (async) returnValue = TRY(Await(returnValue));
        owner->suspend.returnValue = returnValue;
        return returnMarker();
      }
      Value innerReturnResult = TRY(Call(returnMethod, Value(iterator), {received.value}));
      if (async) innerReturnResult = TRY(Await(innerReturnResult));
      if (!innerReturnResult.isObject()) return ThrowTypeError("Iterator result is not an object");
      bool done = TRY(IteratorComplete(innerReturnResult.asObject()));
      if (done) {
        Value returnValue = TRY(IteratorValue(innerReturnResult.asObject()));
        if (async) returnValue = TRY(Await(returnValue));
        owner->suspend.returnValue = returnValue;
        return returnMarker();
      }
      received = TRY(yieldOn(innerReturnResult.asObject()));
    }
  }
}

// ---- Async iteration ----

// 7.4.13 AsyncIteratorClose
Result<void> AsyncIteratorClose(const IteratorRecord& record, Result<void> completion) {
  Object* iterator = record.iterator;
  Result<Value> innerResult = GetMethod(Value(iterator), PropertyKey(u"return"));
  if (innerResult.isNormal()) {
    Value returnMethod = innerResult.value();
    if (returnMethod.isUndefined()) return completion;
    innerResult = Call(returnMethod, Value(iterator));
    if (innerResult.isNormal()) innerResult = Await(innerResult.value());
  }
  if (completion.isThrow()) return completion;
  if (innerResult.isThrow()) return Throw{innerResult.thrown()};
  if (!innerResult.value().isObject()) return ThrowTypeError("iterator.return() did not return an object");
  return completion;
}

Result<IteratorRecord> GetAsyncIterator(Value object) { return GetIterator(object, IteratorKind::Async); }

}  // namespace snowman::interp
