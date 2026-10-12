// Bodies that suspend: generators (27.5), async functions (27.7), async generators (27.6) and module top-level
// await. Each runs its body on a coroutine; yield and await switch back to the resumer.
#pragma once

#include <deque>
#include <memory>

#include "interp/coroutine.h"
#include "interp/interpreter.h"
#include "interp/promise.h"

namespace snowman::interp {

class SuspendableObject : public Object {
 public:
  enum class Kind : uint8_t { Generator, AsyncFunction, AsyncGenerator, Module };
  // The body when it is not an ECMAScript function's: module evaluation, built-in closures.
  using NativeBody = Result<Value> (*)(SuspendableObject* self);

  SuspendableObject(Object* prototype, Kind kind) : Object(prototype), kind(kind) {}
  void trace(Tracer& tracer) override;

  // Starts or resumes the body with a completion. Returns when the body suspends or finishes (finished).
  void resumeBody(ResumeKind resumeKind, Value value);
  // Inside the body: switch back to the resumer. The completion the body is resumed with is in suspend.
  void suspendBody();

  Kind kind;
  InterpContext context;  // the body's execution context (genContext / asyncContext)
  SuspendState suspend;
  std::unique_ptr<Coroutine> coroutine;
  ECMAScriptFunction* function = nullptr;
  NativeBody nativeBody = nullptr;
  Cell* nativeData = nullptr;  // for nativeBody
  // The body's completion, once finished.
  bool finished = false;
  bool outcomeIsThrow = false;
  Value outcome;
};

class GeneratorObject final : public SuspendableObject {
 public:
  enum class State : uint8_t { SuspendedStart, SuspendedYield, Executing, Completed };
  explicit GeneratorObject(Object* prototype) : SuspendableObject(prototype, Kind::Generator) {}
  void trace(Tracer& tracer) override;

  State state = State::SuspendedStart;
  Value brand = Value::empty();  // [[GeneratorBrand]]
};

class AsyncGeneratorObject final : public SuspendableObject {
 public:
  enum class State : uint8_t { SuspendedStart, SuspendedYield, Executing, DrainingQueue, Completed };
  struct Request {
    ResumeKind kind;
    Value value;
    PromiseCapability* capability;
  };
  explicit AsyncGeneratorObject(Object* prototype) : SuspendableObject(prototype, Kind::AsyncGenerator) {}
  void trace(Tracer& tracer) override;

  State state = State::SuspendedStart;
  std::deque<Request> queue;  // [[AsyncGeneratorQueue]]
};

// An async function's (or a module's top-level await) running state; never exposed to JS.
class AsyncState final : public SuspendableObject {
 public:
  explicit AsyncState(Kind kind) : SuspendableObject(nullptr, kind) {}
  void trace(Tracer& tracer) override;

  PromiseCapability* capability = nullptr;
};

// %GeneratorPrototype% next / return / throw: GeneratorResume and GeneratorResumeAbrupt.
Result<Value> GeneratorResume(Value generator, Value value, Value brand);
Result<Value> GeneratorResumeAbrupt(Value generator, ResumeKind kind, Value value, Value brand);
// %AsyncGeneratorPrototype% next / return / throw.
Value AsyncGeneratorNext(Value generator, Value value);
Value AsyncGeneratorReturn(Value generator, Value value);
Value AsyncGeneratorThrow(Value generator, Value exception);

// AsyncBlockStart for a native body (module top-level await): runs body on a coroutine until it awaits or
// finishes, and settles capability with its completion.
void AsyncNativeBlockStart(PromiseCapability* capability, SuspendableObject::NativeBody body, Cell* data,
                           InterpContext& context);

}  // namespace snowman::interp
