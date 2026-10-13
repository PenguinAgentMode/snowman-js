// %IteratorPrototype%, %AsyncIteratorPrototype%, the Iterator constructor and the iterator helpers (27.1).
#include <cmath>

#include "builtins/util.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

// %IteratorPrototype% [ @@iterator ] ( ), %AsyncIteratorPrototype% [ @@asyncIterator ] ( )
Result<Value> returnThis(NativeCall& call) { return call.thisValue; }

// 27.1.3.1 Iterator ( )
Result<Value> Iterator_construct(NativeCall& call) {
  if (!call.newTarget || call.newTarget == call.callee) return ThrowTypeError("Iterator is an abstract class");
  return Value(TRY(OrdinaryCreateFromConstructor<Object>(call.newTarget, Intrinsic::IteratorPrototype)));
}

// 27.1.4.2 GetIteratorFlattenable ( obj, primitiveHandling )
Result<IteratorRecord> GetIteratorFlattenable(Value obj, bool iterateStringPrimitives) {
  if (!obj.isObject()) {
    if (!iterateStringPrimitives || !obj.isString()) return ThrowTypeError("value is not an object");
  }
  Value method = TRY(GetMethod(obj, PropertyKey(symbols().iterator)));
  Value iterator;
  if (method.isUndefined()) iterator = obj;
  else iterator = TRY(Call(method, obj));
  if (!iterator.isObject()) return ThrowTypeError("iterator is not an object");
  return GetIteratorDirect(iterator.asObject());
}

// [[Iterated]] of %WrapForValidIteratorPrototype% objects.
class WrappedIterator : public Object {
 public:
  WrappedIterator(Object* prototype, IteratorRecord iterated) : Object(prototype), iterated(iterated) {}
  IteratorRecord iterated;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(reinterpret_cast<Cell*>(iterated.iterator));
    tracer.visit(iterated.nextMethod);
  }
};

// 27.1.3.2.1 Iterator.from ( O )
Result<Value> Iterator_from(NativeCall& call) {
  IteratorRecord record = TRY(GetIteratorFlattenable(call.args[0], true));
  bool hasInstance = TRY(OrdinaryHasInstance(intrinsic(Intrinsic::Iterator), record.iterator));
  if (hasInstance) return Value(record.iterator);
  return Value(make<WrappedIterator>(intrinsic(Intrinsic::WrapForValidIteratorPrototype), record));
}

Result<WrappedIterator*> thisWrapped(Value v) {
  if (v.isObject()) {
    if (auto* w = v.asObject()->as<WrappedIterator>()) return w;
  }
  return ThrowTypeError("not a wrapped iterator");
}

// 27.1.3.2.1.1.1 %WrapForValidIteratorPrototype%.next ( )
Result<Value> WrapForValid_next(NativeCall& call) {
  WrappedIterator* o = TRY(thisWrapped(call.thisValue));
  return Call(o->iterated.nextMethod, o->iterated.iterator);
}

// 27.1.3.2.1.1.2 %WrapForValidIteratorPrototype%.return ( )
Result<Value> WrapForValid_return(NativeCall& call) {
  WrappedIterator* o = TRY(thisWrapped(call.thisValue));
  Object* iterator = o->iterated.iterator;
  Value returnMethod = TRY(GetMethod(iterator, PropertyKey(u"return")));
  if (returnMethod.isUndefined()) return Value(CreateIteratorResultObject(Value::undefined(), true));
  return Call(returnMethod, iterator);
}

// An iterator helper (27.1.2.1): the generator-like state machine behind map/filter/take/drop/flatMap.
enum class HelperKind { Map, Filter, Take, Drop, FlatMap, Concat };
enum class HelperState { SuspendedStart, SuspendedYield, Executing, Completed };

class IteratorHelper : public Object {
 public:
  IteratorHelper(Object* prototype, HelperKind kind, IteratorRecord underlying, Value fn, double limit)
      : Object(prototype), kind(kind), underlying(underlying), fn(fn), remaining(limit) {}
  HelperKind kind;
  HelperState state = HelperState::SuspendedStart;
  IteratorRecord underlying;
  Value fn;
  double counter = 0;
  double remaining;
  bool dropped = false;
  IteratorRecord inner;
  bool innerAlive = false;
  std::vector<Value> iterables;  // Concat: [openMethod, iterable] pairs
  size_t nextIterable = 0;

  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(reinterpret_cast<Cell*>(underlying.iterator));
    tracer.visit(underlying.nextMethod);
    tracer.visit(fn);
    tracer.visit(reinterpret_cast<Cell*>(inner.iterator));
    tracer.visit(inner.nextMethod);
    tracer.visitAll(iterables);
  }
};

// Runs the helper's closure up to its next Yield: the yielded value, or nullopt when the closure returns.
Result<std::optional<Value>> helperStep(IteratorHelper* h) {
  IteratorRecord& iterated = h->underlying;
  switch (h->kind) {
    case HelperKind::Map: {
      std::optional<Value> value = TRY(IteratorStepValue(iterated));
      if (!value) return std::optional<Value>();
      Value mapped = TRY_CLOSE(iterated, Call(h->fn, Value::undefined(), {*value, num(h->counter)}));
      h->counter++;
      return std::optional<Value>(mapped);
    }
    case HelperKind::Filter: {
      while (true) {
        std::optional<Value> value = TRY(IteratorStepValue(iterated));
        if (!value) return std::optional<Value>();
        Value selected = TRY_CLOSE(iterated, Call(h->fn, Value::undefined(), {*value, num(h->counter)}));
        h->counter++;
        if (ToBoolean(selected)) return std::optional<Value>(*value);
      }
    }
    case HelperKind::Take: {
      if (h->remaining == 0) {
        TRY(IteratorClose(iterated, Result<void>()));
        return std::optional<Value>();
      }
      if (h->remaining != INFINITY) h->remaining--;
      std::optional<Value> value = TRY(IteratorStepValue(iterated));
      if (!value) return std::optional<Value>();
      return std::optional<Value>(*value);
    }
    case HelperKind::Drop: {
      if (!h->dropped) {
        while (h->remaining > 0) {
          if (h->remaining != INFINITY) h->remaining--;
          Object* next = TRY(IteratorStep(iterated));
          if (!next) return std::optional<Value>();
        }
        h->dropped = true;
      }
      std::optional<Value> value = TRY(IteratorStepValue(iterated));
      if (!value) return std::optional<Value>();
      return std::optional<Value>(*value);
    }
    case HelperKind::Concat: {
      while (true) {
        if (h->innerAlive) {
          std::optional<Value> innerValue = TRY(IteratorStepValue(h->inner));
          if (innerValue) return std::optional<Value>(*innerValue);
          h->innerAlive = false;
        }
        if (h->nextIterable * 2 >= h->iterables.size()) return std::optional<Value>();
        Value method = h->iterables[h->nextIterable * 2], iterable = h->iterables[h->nextIterable * 2 + 1];
        h->nextIterable++;
        Value iter = TRY(Call(method, iterable));
        if (!iter.isObject()) return ThrowTypeError("Iterator.concat: the iterator method did not return an object");
        h->inner = TRY(GetIteratorDirect(iter.asObject()));
        h->innerAlive = true;
      }
    }
    case HelperKind::FlatMap: {
      while (true) {
        if (h->innerAlive) {
          std::optional<Value> innerValue = TRY_CLOSE(iterated, IteratorStepValue(h->inner));
          if (innerValue) return std::optional<Value>(*innerValue);
          h->innerAlive = false;
          continue;
        }
        std::optional<Value> value = TRY(IteratorStepValue(iterated));
        if (!value) return std::optional<Value>();
        Value mapped = TRY_CLOSE(iterated, Call(h->fn, Value::undefined(), {*value, num(h->counter)}));
        h->inner = TRY_CLOSE(iterated, GetIteratorFlattenable(mapped, false));
        h->innerAlive = true;
        h->counter++;
      }
    }
  }
  return std::optional<Value>();
}

Result<IteratorHelper*> thisHelper(Value v) {
  if (v.isObject()) {
    if (auto* h = v.asObject()->as<IteratorHelper>()) return h;
  }
  return ThrowTypeError("not an Iterator Helper");
}

// 27.1.2.1.1 %IteratorHelperPrototype%.next ( )
Result<Value> IteratorHelper_next(NativeCall& call) {
  IteratorHelper* h = TRY(thisHelper(call.thisValue));
  if (h->state == HelperState::Executing) return ThrowTypeError("Iterator Helper is already running");
  if (h->state == HelperState::Completed) return Value(CreateIteratorResultObject(Value::undefined(), true));
  h->state = HelperState::Executing;
  Result<std::optional<Value>> r = helperStep(h);
  if (r.isThrow()) {
    h->state = HelperState::Completed;
    return Throw{r.thrown()};
  }
  if (!r.value()) {
    h->state = HelperState::Completed;
    return Value(CreateIteratorResultObject(Value::undefined(), true));
  }
  h->state = HelperState::SuspendedYield;
  return Value(CreateIteratorResultObject(*r.value(), false));
}

// The closure's response to a return completion at its Yield: close what it is iterating.
Result<void> closeHelperIterators(IteratorHelper* h) {
  if (h->kind == HelperKind::Concat) {
    if (h->innerAlive) TRY(IteratorClose(h->inner, Result<void>()));
    return {};
  }
  if (h->kind == HelperKind::FlatMap && h->innerAlive) {
    Result<void> backup = IteratorClose(h->inner, Result<void>());
    if (backup.isThrow()) return closeIteratorWithThrow(h->underlying, backup.thrown());
  }
  return IteratorClose(h->underlying, Result<void>());
}

// 27.1.2.1.2 %IteratorHelperPrototype%.return ( )
Result<Value> IteratorHelper_return(NativeCall& call) {
  IteratorHelper* h = TRY(thisHelper(call.thisValue));
  if (h->state == HelperState::SuspendedStart) {
    h->state = HelperState::Completed;
    if (h->kind != HelperKind::Concat) TRY(IteratorClose(h->underlying, Result<void>()));
    return Value(CreateIteratorResultObject(Value::undefined(), true));
  }
  if (h->state == HelperState::Completed) return Value(CreateIteratorResultObject(Value::undefined(), true));
  if (h->state == HelperState::Executing) return ThrowTypeError("Iterator Helper is already running");
  // GeneratorResumeAbrupt: the closure runs (closing its iterators) in the executing state.
  h->state = HelperState::Executing;
  Result<void> closed = closeHelperIterators(h);
  h->state = HelperState::Completed;
  if (closed.isThrow()) return Throw{closed.thrown()};
  return Value(CreateIteratorResultObject(Value::undefined(), true));
}

// The argument checks shared by the lazy helpers: a non-object receiver throws; a bad argument closes it.
Result<Value> makeHelper(NativeCall& call, HelperKind kind) {
  Value o = call.thisValue;
  if (!o.isObject()) return ThrowTypeError("Iterator helper called on non-object");
  IteratorRecord iterated{o.asObject(), Value::undefined(), false};
  Value fn;
  double limit = 0;
  if (kind == HelperKind::Take || kind == HelperKind::Drop) {
    double numLimit = TRY_CLOSE(iterated, ToNumber(call.args[0]));
    if (std::isnan(numLimit)) return closeIteratorWithThrow(iterated, ThrowRangeError("limit must be a number").value);
    limit = TRY(ToIntegerOrInfinity(num(numLimit)));
    if (limit < 0 || (limit != INFINITY && limit > 9007199254740991.0))
      return closeIteratorWithThrow(iterated, ThrowRangeError("limit must be between 0 and 2^53 - 1").value);
  } else {
    fn = call.args[0];
    if (!IsCallable(fn)) return closeIteratorWithThrow(iterated, ThrowTypeError("callback is not a function").value);
  }
  iterated = TRY(GetIteratorDirect(o.asObject()));
  return Value(make<IteratorHelper>(intrinsic(Intrinsic::IteratorHelperPrototype), kind, iterated, fn, limit));
}

Result<Value> IteratorPrototype_map(NativeCall& call) { return makeHelper(call, HelperKind::Map); }
Result<Value> IteratorPrototype_filter(NativeCall& call) { return makeHelper(call, HelperKind::Filter); }
Result<Value> IteratorPrototype_take(NativeCall& call) { return makeHelper(call, HelperKind::Take); }
Result<Value> IteratorPrototype_drop(NativeCall& call) { return makeHelper(call, HelperKind::Drop); }
Result<Value> IteratorPrototype_flatMap(NativeCall& call) { return makeHelper(call, HelperKind::FlatMap); }

Result<IteratorRecord> eagerStart(NativeCall& call, bool needsCallable) {
  Value o = call.thisValue;
  if (!o.isObject()) return ThrowTypeError("Iterator helper called on non-object");
  IteratorRecord iterated{o.asObject(), Value::undefined(), false};
  if (needsCallable && !IsCallable(call.args[0]))
    return closeIteratorWithThrow(iterated, ThrowTypeError("callback is not a function").value);
  return GetIteratorDirect(o.asObject());
}

// Iterator.concat ( ...items )
Result<Value> Iterator_concat(NativeCall& call) {
  std::vector<Value> iterables;
  ValueVector rooted;
  for (Value item : call.args) {
    if (!item.isObject()) return ThrowTypeError("Iterator.concat: argument is not an object");
    Value method = TRY(GetMethod(item, PropertyKey(symbols().iterator)));
    if (method.isUndefined()) return ThrowTypeError("Iterator.concat: argument is not iterable");
    iterables.push_back(method);
    iterables.push_back(item);
    rooted.push_back(method);
  }
  IteratorHelper* gen = make<IteratorHelper>(intrinsic(Intrinsic::IteratorHelperPrototype), HelperKind::Concat,
                                             IteratorRecord{}, Value::undefined(), 0);
  gen->iterables = std::move(iterables);
  return Value(gen);
}

// 27.1.4.8 Iterator.prototype.reduce ( reducer [ , initialValue ] )
Result<Value> IteratorPrototype_reduce(NativeCall& call) {
  IteratorRecord iterated = TRY(eagerStart(call, true));
  Value reducer = call.args[0];
  Value accumulator;
  double counter;
  if (call.args.size() < 2) {
    std::optional<Value> first = TRY(IteratorStepValue(iterated));
    if (!first) return ThrowTypeError("Reduce of empty iterator with no initial value");
    accumulator = *first;
    counter = 1;
  } else {
    accumulator = call.args[1];
    counter = 0;
  }
  while (true) {
    std::optional<Value> value = TRY(IteratorStepValue(iterated));
    if (!value) return accumulator;
    accumulator = TRY_CLOSE(iterated, Call(reducer, Value::undefined(), {accumulator, *value, num(counter)}));
    counter++;
  }
}

// Iterator.prototype.includes ( searchElement [ , skippedElements ] )
Result<Value> IteratorPrototype_includes(NativeCall& call) {
  Value o = call.thisValue;
  if (!o.isObject()) return ThrowTypeError("Iterator.prototype.includes called on non-object");
  IteratorRecord iterated{o.asObject(), Value::undefined(), false};
  double toSkip = 0;
  Value skipped = call.args[1];
  if (!skipped.isUndefined()) {
    if (!skipped.isNumber() || std::isnan(skipped.asNumber()) ||
        (std::isfinite(skipped.asNumber()) && !IsIntegralNumber(skipped)))
      return closeIteratorWithThrow(iterated, ThrowTypeError("skippedElements must be an integral number").value);
    toSkip = skipped.asNumber();
    if (toSkip < 0 || (toSkip != INFINITY && toSkip > 9007199254740991.0))
      return closeIteratorWithThrow(iterated, ThrowRangeError("skippedElements is out of range").value);
  }
  iterated = TRY(GetIteratorDirect(o.asObject()));
  double count = 0;
  while (true) {
    std::optional<Value> value = TRY(IteratorStepValue(iterated));
    if (!value) return boolean(false);
    if (count < toSkip) {
      count++;
      continue;
    }
    if (SameValueZero(*value, call.args[0])) {
      TRY(IteratorClose(iterated, Result<void>()));
      return boolean(true);
    }
  }
}

// 27.1.4.12 Iterator.prototype.toArray ( )
Result<Value> IteratorPrototype_toArray(NativeCall& call) {
  IteratorRecord iterated = TRY(eagerStart(call, false));
  ValueVector items;
  while (true) {
    std::optional<Value> value = TRY(IteratorStepValue(iterated));
    if (!value) return Value(CreateArrayFromList(items));
    items.push_back(*value);
  }
}

// 27.1.4.5 forEach, 27.1.4.10 some, 27.1.4.4 every, 27.1.4.6 find
enum class EagerKind { ForEach, Some, Every, Find };

Result<Value> eager(NativeCall& call, EagerKind kind) {
  IteratorRecord iterated = TRY(eagerStart(call, true));
  Value fn = call.args[0];
  double counter = 0;
  while (true) {
    std::optional<Value> value = TRY(IteratorStepValue(iterated));
    if (!value) {
      switch (kind) {
        case EagerKind::ForEach:
        case EagerKind::Find:
          return Value::undefined();
        case EagerKind::Some:
          return boolean(false);
        case EagerKind::Every:
          return boolean(true);
      }
    }
    Value result = TRY_CLOSE(iterated, Call(fn, Value::undefined(), {*value, num(counter)}));
    switch (kind) {
      case EagerKind::ForEach:
        break;
      case EagerKind::Some:
        if (ToBoolean(result)) {
          TRY(IteratorClose(iterated, Result<void>()));
          return boolean(true);
        }
        break;
      case EagerKind::Every:
        if (!ToBoolean(result)) {
          TRY(IteratorClose(iterated, Result<void>()));
          return boolean(false);
        }
        break;
      case EagerKind::Find:
        if (ToBoolean(result)) {
          TRY(IteratorClose(iterated, Result<void>()));
          return *value;
        }
        break;
    }
    counter++;
  }
}

Result<Value> IteratorPrototype_forEach(NativeCall& call) { return eager(call, EagerKind::ForEach); }
Result<Value> IteratorPrototype_some(NativeCall& call) { return eager(call, EagerKind::Some); }
Result<Value> IteratorPrototype_every(NativeCall& call) { return eager(call, EagerKind::Every); }
Result<Value> IteratorPrototype_find(NativeCall& call) { return eager(call, EagerKind::Find); }

// 27.1.4.14.1 SetterThatIgnoresPrototypeProperties ( thisValue, home, p, v )
Result<void> SetterThatIgnoresPrototypeProperties(Value thisValue, Object* home, const PropertyKey& p, Value v) {
  if (!thisValue.isObject()) return ThrowTypeError("setter called on non-object");
  if (thisValue.asObject() == home) return ThrowTypeError("Cannot assign to read only property");
  auto desc = TRY(thisValue.asObject()->GetOwnProperty(p));
  if (!desc) TRY(CreateDataPropertyOrThrow(thisValue.asObject(), p, v));
  else TRY(Set(thisValue.asObject(), p, v, true));
  return {};
}

Result<Value> IteratorPrototype_getToStringTag(NativeCall&) { return str(u"Iterator"); }
Result<Value> IteratorPrototype_setToStringTag(NativeCall& call) {
  TRY(SetterThatIgnoresPrototypeProperties(call.thisValue, intrinsic(Intrinsic::IteratorPrototype),
                                           PropertyKey(symbols().toStringTag), call.args[0]));
  return Value::undefined();
}
Result<Value> IteratorPrototype_getConstructor(NativeCall&) { return Value(intrinsic(Intrinsic::Iterator)); }
Result<Value> IteratorPrototype_setConstructor(NativeCall& call) {
  TRY(SetterThatIgnoresPrototypeProperties(call.thisValue, intrinsic(Intrinsic::IteratorPrototype),
                                           PropertyKey(u"constructor"), call.args[0]));
  return Value::undefined();
}

void installIterator(Realm& realm) {
  Object* objectProto = realm.intrinsic(Intrinsic::ObjectPrototype);
  Object* proto = OrdinaryObjectCreate(objectProto);
  realm.setIntrinsic(Intrinsic::IteratorPrototype, proto);
  defineFunction(realm, proto, PropertyKey(symbols().iterator), 0, returnThis);

  Object* asyncProto = OrdinaryObjectCreate(objectProto);
  realm.setIntrinsic(Intrinsic::AsyncIteratorPrototype, asyncProto);
  defineFunction(realm, asyncProto, PropertyKey(symbols().asyncIterator), 0, returnThis);

  BuiltinFunction* ctor = createBuiltinFunction(realm, Iterator_construct, 0, PropertyKey(u"Iterator"), true);
  defineValue(ctor, PropertyKey(u"prototype"), proto, Attr::None);
  realm.setIntrinsic(Intrinsic::Iterator, ctor);
  defineGlobal(realm, PropertyKey(u"Iterator"), ctor);
  defineFunction(realm, ctor, PropertyKey(u"concat"), 0, Iterator_concat);
  defineFunction(realm, ctor, PropertyKey(u"from"), 1, Iterator_from);

  defineAccessor(realm, proto, PropertyKey(u"constructor"), IteratorPrototype_getConstructor, IteratorPrototype_setConstructor);
  defineFunction(realm, proto, PropertyKey(u"drop"), 1, IteratorPrototype_drop);
  defineFunction(realm, proto, PropertyKey(u"every"), 1, IteratorPrototype_every);
  defineFunction(realm, proto, PropertyKey(u"filter"), 1, IteratorPrototype_filter);
  defineFunction(realm, proto, PropertyKey(u"find"), 1, IteratorPrototype_find);
  defineFunction(realm, proto, PropertyKey(u"flatMap"), 1, IteratorPrototype_flatMap);
  defineFunction(realm, proto, PropertyKey(u"forEach"), 1, IteratorPrototype_forEach);
  defineFunction(realm, proto, PropertyKey(u"includes"), 1, IteratorPrototype_includes);
  defineFunction(realm, proto, PropertyKey(u"map"), 1, IteratorPrototype_map);
  defineFunction(realm, proto, PropertyKey(u"reduce"), 1, IteratorPrototype_reduce);
  defineFunction(realm, proto, PropertyKey(u"some"), 1, IteratorPrototype_some);
  defineFunction(realm, proto, PropertyKey(u"take"), 1, IteratorPrototype_take);
  defineFunction(realm, proto, PropertyKey(u"toArray"), 0, IteratorPrototype_toArray);
  defineAccessor(realm, proto, PropertyKey(symbols().toStringTag), IteratorPrototype_getToStringTag,
                 IteratorPrototype_setToStringTag);

  Object* helperProto = OrdinaryObjectCreate(proto);
  realm.setIntrinsic(Intrinsic::IteratorHelperPrototype, helperProto);
  defineFunction(realm, helperProto, PropertyKey(u"next"), 0, IteratorHelper_next);
  defineFunction(realm, helperProto, PropertyKey(u"return"), 0, IteratorHelper_return);
  defineValue(helperProto, PropertyKey(symbols().toStringTag), str(u"Iterator Helper"), Attr::Configurable);

  Object* wrapProto = OrdinaryObjectCreate(proto);
  realm.setIntrinsic(Intrinsic::WrapForValidIteratorPrototype, wrapProto);
  defineFunction(realm, wrapProto, PropertyKey(u"next"), 0, WrapForValid_next);
  defineFunction(realm, wrapProto, PropertyKey(u"return"), 0, WrapForValid_return);
}

}  // namespace

SNOWMAN_BUILTIN(Iterator, kOrderFundamental, installIterator);

}  // namespace snowman::builtins
