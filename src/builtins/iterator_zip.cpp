// Iterator.zip ( iterables [ , options ] ) and Iterator.zipKeyed ( iterables [ , options ] )
// (proposal-joint-iteration): IteratorZip's Iterator Helper over several iterators.
#include <vector>

#include "builtins/iterator_closure_helper.h"

namespace snowman::builtins {
namespace {

enum class ZipMode { Shortest, Longest, Strict };

class ZipHelper : public ClosureIteratorHelper {
 public:
  // keyed: the results are objects with these keys (zipKeyed), else arrays.
  ZipHelper(std::vector<IteratorRecord> iters, ZipMode mode, std::vector<Value> padding, bool keyed, std::vector<PropertyKey> keys)
      : iters(std::move(iters)), open(this->iters.size(), true), exhausted(this->iters.size(), false), mode(mode),
        padding(std::move(padding)), keyed(keyed), keys(std::move(keys)) {}

  // One round of IteratorZip's closure: a value from each iterator (or its padding), then Yield.
  Result<std::optional<Value>> resume() override {
    size_t count = iters.size();
    if (count == 0) return std::optional<Value>();
    ValueVector results;
    for (size_t i = 0; i < count; i++) {
      Value result;
      if (exhausted[i]) {
        result = padding[i];  // longest mode
      } else {
        Result<std::optional<Value>> step = IteratorStepValue(iters[i]);
        if (step.isThrow()) {
          open[i] = false;
          return closeAll(Throw{step.thrown()});
        }
        if (step.value()) {
          result = *step.value();
        } else {
          open[i] = false;
          if (mode == ZipMode::Shortest) {
            TRY(closeAll(Result<void>()));
            return std::optional<Value>();
          }
          if (mode == ZipMode::Strict) {
            if (i != 0) return closeAll(ThrowTypeError("Iterator.zip: iterators have different lengths"));
            for (size_t k = 1; k < count; k++) {
              Result<Object*> next = IteratorStep(iters[k]);
              if (next.isThrow()) {
                open[k] = false;
                return closeAll(Throw{next.thrown()});
              }
              if (next.value()) return closeAll(ThrowTypeError("Iterator.zip: iterators have different lengths"));
              open[k] = false;
            }
            return std::optional<Value>();
          }
          if (openIters().empty()) return std::optional<Value>();
          exhausted[i] = true;
          result = padding[i];
        }
      }
      results.push_back(result);
    }
    return std::optional<Value>(finishResults(results));
  }

  // The Yield's abrupt completion: IteratorCloseAll(openIters, completion).
  Result<void> resumeWithReturn() override { return IteratorCloseAll(openIters(), Result<void>()); }
  Result<void> closeUnderlyingIterators() override { return IteratorCloseAll(openIters(), Result<void>()); }

  void trace(Tracer& tracer) override {
    ClosureIteratorHelper::trace(tracer);
    for (const IteratorRecord& r : iters) {
      tracer.visit(reinterpret_cast<Cell*>(r.iterator));
      tracer.visit(r.nextMethod);
    }
    for (const Value& v : padding) tracer.visit(v);
    for (const PropertyKey& k : keys) k.trace(tracer);
  }

 private:
  // openIters: the iterators not yet removed, in their original order.
  std::vector<IteratorRecord> openIters() const {
    std::vector<IteratorRecord> list;
    for (size_t i = 0; i < iters.size(); i++) {
      if (open[i]) list.push_back(iters[i]);
    }
    return list;
  }
  // Return ? IteratorCloseAll(openIters, completion), as a closure's return: done, or the throw.
  Result<std::optional<Value>> closeAll(Result<void> completion) {
    TRY(IteratorCloseAll(openIters(), std::move(completion)));
    return std::optional<Value>();
  }
  Throw closeAll(Throw thrown) {
    Result<void> r = IteratorCloseAll(openIters(), thrown);
    return Throw{r.thrown()};
  }
  Value finishResults(const ValueVector& results) {
    if (!keyed) return Value(CreateArrayFromList(results));
    Object* obj = OrdinaryObjectCreate(nullptr);
    for (size_t i = 0; i < results.size(); i++) (void)CreateDataPropertyOrThrow(obj, keys[i], results[i]);
    return Value(obj);
  }

  std::vector<IteratorRecord> iters;
  std::vector<bool> open;
  std::vector<bool> exhausted;  // iters[i] is null: done, padded (longest)
  ZipMode mode;
  std::vector<Value> padding;
  bool keyed;
  std::vector<PropertyKey> keys;
};

// GetOptionsObject ( options )
Result<Object*> GetOptionsObject(Value options) {
  if (options.isUndefined()) return OrdinaryObjectCreate(nullptr);
  if (options.isObject()) return options.asObject();
  return ThrowTypeError("options must be an object");
}

// GetIteratorFlattenable ( obj, reject-primitives )
Result<IteratorRecord> GetIteratorFlattenable(Value obj) {
  if (!obj.isObject()) return ThrowTypeError("Iterator.zip: not an iterable or iterator object");
  Value method = TRY(GetMethod(obj, PropertyKey(symbols().iterator)));
  Value iterator = method.isUndefined() ? obj : TRY(Call(method, obj));
  if (!iterator.isObject()) return ThrowTypeError("Iterator.zip: iterator is not an object");
  return GetIteratorDirect(iterator.asObject());
}

// The iterator records being collected, kept reachable (a std::vector outside a cell is not a GC root).
struct Iters {
  std::vector<IteratorRecord> list;
  ValueVector roots;
  void push(const IteratorRecord& r) {
    list.push_back(r);
    roots.push_back(Value(r.iterator));
    roots.push_back(r.nextMethod);
  }
  // IfAbruptCloseIterators(value, iters)
  Throw closeAll(Value thrown) const { return Throw{IteratorCloseAll(list, Throw{thrown}).thrown()}; }
};

// The options steps both methods share: mode, and padding's option with "longest".
struct ZipOptions {
  ZipMode mode = ZipMode::Shortest;
  Value padding;
};
Result<ZipOptions> readOptions(Value optionsValue) {
  Object* options = TRY(GetOptionsObject(optionsValue));
  Value mode = TRY(Get(options, PropertyKey(u"mode")));
  ZipOptions out;
  if (mode.isUndefined()) {
    out.mode = ZipMode::Shortest;
  } else if (mode.isString() && mode.asString()->view() == u"shortest") {
    out.mode = ZipMode::Shortest;
  } else if (mode.isString() && mode.asString()->view() == u"longest") {
    out.mode = ZipMode::Longest;
  } else if (mode.isString() && mode.asString()->view() == u"strict") {
    out.mode = ZipMode::Strict;
  } else {
    return ThrowTypeError("mode must be \"shortest\", \"longest\" or \"strict\"");
  }
  if (out.mode == ZipMode::Longest) {
    out.padding = TRY(Get(options, PropertyKey(u"padding")));
    if (!out.padding.isUndefined() && !out.padding.isObject()) return ThrowTypeError("padding must be an object");
  }
  return out;
}

// Iterator.zip ( iterables [ , options ] )
Result<Value> Iterator_zip(NativeCall& call) {
  Value iterables = call.args[0];
  if (!iterables.isObject()) return ThrowTypeError("Iterator.zip: iterables must be an object");
  ZipOptions options = TRY(readOptions(call.args[1]));
  Iters iters;
  IteratorRecord inputIter = TRY(GetIterator(iterables, IteratorKind::Sync));
  while (true) {
    Result<std::optional<Value>> next = IteratorStepValue(inputIter);
    if (next.isThrow()) return iters.closeAll(next.thrown());
    if (!next.value()) break;
    Result<IteratorRecord> iter = GetIteratorFlattenable(*next.value());
    if (iter.isThrow()) {
      // IfAbruptCloseIterators(iter, « inputIter » + iters): iters last first, then inputIter.
      Result<void> c = IteratorCloseAll(iters.list, Throw{iter.thrown()});
      return Throw{IteratorClose(inputIter, std::move(c)).thrown()};
    }
    iters.push(iter.value());
  }
  size_t count = iters.list.size();
  std::vector<Value> padding;
  ValueVector paddingRoots;
  if (options.mode == ZipMode::Longest) {
    if (options.padding.isUndefined()) {
      padding.assign(count, Value::undefined());
    } else {
      Result<IteratorRecord> paddingIter = GetIterator(options.padding, IteratorKind::Sync);
      if (paddingIter.isThrow()) return iters.closeAll(paddingIter.thrown());
      bool usingIterator = true;
      for (size_t i = 0; i < count; i++) {
        Value value;
        if (usingIterator) {
          Result<std::optional<Value>> next = IteratorStepValue(paddingIter.value());
          if (next.isThrow()) return iters.closeAll(next.thrown());
          if (next.value()) {
            value = *next.value();
          } else {
            usingIterator = false;
          }
        }
        padding.push_back(value);
        paddingRoots.push_back(value);
      }
      if (usingIterator) {
        Result<void> completion = IteratorClose(paddingIter.value(), Result<void>());
        if (completion.isThrow()) return iters.closeAll(completion.thrown());
      }
    }
  }
  return Value(make<ZipHelper>(std::move(iters.list), options.mode, std::move(padding), false, std::vector<PropertyKey>()));
}

// Iterator.zipKeyed ( iterables [ , options ] )
Result<Value> Iterator_zipKeyed(NativeCall& call) {
  Value iterablesValue = call.args[0];
  if (!iterablesValue.isObject()) return ThrowTypeError("Iterator.zipKeyed: iterables must be an object");
  Object* iterables = iterablesValue.asObject();
  ZipOptions options = TRY(readOptions(call.args[1]));
  Iters iters;
  std::vector<PropertyKey> allKeys = TRY(iterables->OwnPropertyKeys());
  std::vector<PropertyKey> keys;
  for (const PropertyKey& key : allKeys) {
    Result<std::optional<PropertyDescriptor>> desc = iterables->GetOwnProperty(key);
    if (desc.isThrow()) return iters.closeAll(desc.thrown());
    if (!desc.value() || !desc.value()->enumerable.value_or(false)) continue;
    Result<Value> value = Get(iterables, key);
    if (value.isThrow()) return iters.closeAll(value.thrown());
    if (value.value().isUndefined()) continue;
    keys.push_back(key);
    Result<IteratorRecord> iter = GetIteratorFlattenable(value.value());
    if (iter.isThrow()) return iters.closeAll(iter.thrown());
    iters.push(iter.value());
  }
  size_t count = iters.list.size();
  std::vector<Value> padding;
  ValueVector paddingRoots;
  if (options.mode == ZipMode::Longest) {
    if (options.padding.isUndefined()) {
      padding.assign(count, Value::undefined());
    } else {
      for (const PropertyKey& key : keys) {
        Result<Value> value = Get(options.padding.asObject(), key);
        if (value.isThrow()) return iters.closeAll(value.thrown());
        padding.push_back(value.value());
        paddingRoots.push_back(value.value());
      }
    }
  }
  return Value(make<ZipHelper>(std::move(iters.list), options.mode, std::move(padding), true, std::move(keys)));
}

void installIteratorZip(Realm& realm) {
  Object* ctor = realm.intrinsic(Intrinsic::Iterator);
  defineFunction(realm, ctor, PropertyKey(u"zip"), 1, Iterator_zip);
  defineFunction(realm, ctor, PropertyKey(u"zipKeyed"), 1, Iterator_zipKeyed);
}

}  // namespace

SNOWMAN_BUILTIN(IteratorZip, kOrderLate, installIteratorZip);

}  // namespace snowman::builtins
