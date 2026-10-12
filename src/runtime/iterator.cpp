// 7.4 Operations on iterator objects (the synchronous ones).
#include "runtime/operations.h"
#include "runtime/property_map.h"
#include "runtime/symbol.h"
#include "runtime/vm.h"

namespace snowman {

// 7.4.2 GetIteratorFromMethod
Result<IteratorRecord> GetIteratorFromMethod(Value object, Value method) {
  Value iterator = TRY(Call(method, object));
  if (!iterator.isObject()) return ThrowTypeError("Result of the Symbol.iterator method is not an object");
  Value nextMethod = TRY(Get(iterator.asObject(), PropertyKey(u"next")));
  return IteratorRecord{iterator.asObject(), nextMethod, false};
}

// Weak default so the runtime links before src/interp/ provides %AsyncFromSyncIteratorPrototype%.
__attribute__((weak)) Result<IteratorRecord> CreateAsyncFromSyncIterator(const IteratorRecord&) {
  return ThrowTypeError("async-from-sync iterators are not supported");
}

// 7.4.3 GetIterator
Result<IteratorRecord> GetIterator(Value object, IteratorKind kind) {
  if (kind == IteratorKind::Async) {
    Value method = TRY(GetMethod(object, PropertyKey(vm().symbols.asyncIterator)));
    if (method.isUndefined()) {
      Value syncMethod = TRY(GetMethod(object, PropertyKey(vm().symbols.iterator)));
      if (syncMethod.isUndefined()) return ThrowTypeError("object is not async iterable");
      IteratorRecord syncIteratorRecord = TRY(GetIteratorFromMethod(object, syncMethod));
      return CreateAsyncFromSyncIterator(syncIteratorRecord);
    }
    return GetIteratorFromMethod(object, method);
  }
  Value method = TRY(GetMethod(object, PropertyKey(vm().symbols.iterator)));
  if (method.isUndefined()) return ThrowTypeError("object is not iterable");
  return GetIteratorFromMethod(object, method);
}

// 7.4.1 GetIteratorDirect
Result<IteratorRecord> GetIteratorDirect(Object* object) {
  Value nextMethod = TRY(Get(object, PropertyKey(u"next")));
  return IteratorRecord{object, nextMethod, false};
}

// 7.4.4 IteratorNext
Result<Object*> IteratorNext(IteratorRecord& record, std::optional<Value> value) {
  Result<Value> result = value ? Call(record.nextMethod, record.iterator, {*value}) : Call(record.nextMethod, record.iterator);
  if (result.isThrow()) {
    record.done = true;
    return Throw{result.thrown()};
  }
  if (!result.value().isObject()) {
    record.done = true;
    return ThrowTypeError("Iterator result is not an object");
  }
  return result.value().asObject();
}

// 7.4.5 IteratorComplete
Result<bool> IteratorComplete(Object* iteratorResult) {
  return ToBoolean(TRY(Get(iteratorResult, PropertyKey(u"done"))));
}

// 7.4.6 IteratorValue
Result<Value> IteratorValue(Object* iteratorResult) { return Get(iteratorResult, PropertyKey(u"value")); }

// 7.4.7 IteratorStep
Result<Object*> IteratorStep(IteratorRecord& record) {
  Object* result = TRY(IteratorNext(record));
  Result<bool> done = IteratorComplete(result);
  if (done.isThrow()) {
    record.done = true;
    return Throw{done.thrown()};
  }
  if (done.value()) {
    record.done = true;
    return static_cast<Object*>(nullptr);
  }
  return result;
}

// 7.4.8 IteratorStepValue
Result<std::optional<Value>> IteratorStepValue(IteratorRecord& record) {
  Object* result = TRY(IteratorStep(record));
  if (!result) return std::optional<Value>();
  Result<Value> value = IteratorValue(result);
  if (value.isThrow()) {
    record.done = true;
    return Throw{value.thrown()};
  }
  return std::optional<Value>(value.value());
}

// 7.4.9 IteratorClose
Result<void> IteratorClose(const IteratorRecord& record, Result<void> completion) {
  Object* iterator = record.iterator;
  Result<Value> innerResult = GetMethod(iterator, PropertyKey(u"return"));
  if (innerResult.isNormal()) {
    Value returnMethod = innerResult.value();
    if (returnMethod.isUndefined()) return completion;
    innerResult = Call(returnMethod, iterator);
  }
  if (completion.isThrow()) return completion;
  if (innerResult.isThrow()) return Throw{innerResult.thrown()};
  if (!innerResult.value().isObject()) return ThrowTypeError("iterator.return() did not return an object");
  return completion;
}

// 7.4.14 CreateIteratorResultObject
Object* CreateIteratorResultObject(Value value, bool done) {
  Object* obj = OrdinaryObjectCreate(vm().currentRealm()->intrinsic(Intrinsic::ObjectPrototype));
  obj->properties().put(PropertyKey(u"value"), PropertySlot::data(value, Attr::All));
  obj->properties().put(PropertyKey(u"done"), PropertySlot::data(Value::boolean(done), Attr::All));
  return obj;
}

// 7.4.15 IteratorToList
Result<ValueVector> IteratorToList(IteratorRecord& record) {
  ValueVector values;
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(record));
    if (!next) return values;
    values.push_back(*next);
  }
}

}  // namespace snowman
