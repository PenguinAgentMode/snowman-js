// %IteratorHelperPrototype%.next / .return for ClosureIteratorHelper objects (iterator_closure_helper.h). The
// generator states are GeneratorValidate / GeneratorResume / GeneratorResumeAbrupt's, with the closure run by
// resume(). Other receivers go to the methods iterator.cpp installed (kept in the replacement's slots[0]).
#include "builtins/iterator_closure_helper.h"

namespace snowman::builtins {

ClosureIteratorHelper::ClosureIteratorHelper() : Object(intrinsic(Intrinsic::IteratorHelperPrototype)) {}

namespace {

ClosureIteratorHelper* asClosureHelper(Value v) {
  return v.isObject() ? v.asObject()->as<ClosureIteratorHelper>() : nullptr;
}

Result<Value> original(NativeCall& call) { return Call(call.callee->slots[0], call.thisValue, call.args); }

// 27.1.2.1.1 %IteratorHelperPrototype%.next ( )
Result<Value> ClosureHelper_next(NativeCall& call) {
  ClosureIteratorHelper* h = asClosureHelper(call.thisValue);
  if (!h) return original(call);
  using State = ClosureIteratorHelper::State;
  if (h->state == State::Executing) return ThrowTypeError("Iterator Helper is already running");
  if (h->state == State::Completed) return Value(CreateIteratorResultObject(Value::undefined(), true));
  h->state = State::Executing;
  Result<std::optional<Value>> r = h->resume();
  if (r.isThrow()) {
    h->state = State::Completed;
    return Throw{r.thrown()};
  }
  if (!r.value()) {
    h->state = State::Completed;
    return Value(CreateIteratorResultObject(Value::undefined(), true));
  }
  h->state = State::SuspendedYield;
  return Value(CreateIteratorResultObject(*r.value(), false));
}

// 27.1.2.1.2 %IteratorHelperPrototype%.return ( )
Result<Value> ClosureHelper_return(NativeCall& call) {
  ClosureIteratorHelper* h = asClosureHelper(call.thisValue);
  if (!h) return original(call);
  using State = ClosureIteratorHelper::State;
  switch (h->state) {
    case State::SuspendedStart:
      h->state = State::Completed;
      TRY(h->closeUnderlyingIterators());
      return Value(CreateIteratorResultObject(Value::undefined(), true));
    case State::Completed:
      return Value(CreateIteratorResultObject(Value::undefined(), true));
    case State::Executing:
      return ThrowTypeError("Iterator Helper is already running");
    case State::SuspendedYield:
      break;
  }
  h->state = State::Executing;
  Result<void> r = h->resumeWithReturn();
  h->state = State::Completed;
  if (r.isThrow()) return Throw{r.thrown()};
  return Value(CreateIteratorResultObject(Value::undefined(), true));
}

void wrap(Realm& realm, Object* proto, const char16_t* name, NativeFn fn) {
  PropertyKey key(name);
  Value previous = proto->GetOwnProperty(key).value()->value.value_or(Value::undefined());
  BuiltinFunction* f = defineFunction(realm, proto, key, 0, fn);
  f->slots.push_back(previous);
}

void installClosureHelpers(Realm& realm) {
  Object* proto = realm.intrinsic(Intrinsic::IteratorHelperPrototype);
  wrap(realm, proto, u"next", ClosureHelper_next);
  wrap(realm, proto, u"return", ClosureHelper_return);
}

}  // namespace

SNOWMAN_BUILTIN(IteratorClosureHelpers, kOrderLate, installClosureHelpers);

}  // namespace snowman::builtins
