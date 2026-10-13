// Iterator.prototype.chunks ( chunkSize ) and Iterator.prototype.windows ( windowSize [ , undersized ] )
// (proposal-iterator-chunking): Iterator Helpers over one underlying iterator.
#include <cmath>
#include <deque>

#include "builtins/iterator_closure_helper.h"

namespace snowman::builtins {
namespace {

class ChunkingHelper : public ClosureIteratorHelper {
 public:
  ChunkingHelper(IteratorRecord iterated, double size, bool windows, bool allowPartial)
      : iterated(iterated), size(size), windows(windows), allowPartial(allowPartial) {}

  // The closures of chunks and windows, between Yields.
  Result<std::optional<Value>> resume() override {
    if (finalYield) return std::optional<Value>();  // after "Perform Completion(Yield(...))": return
    while (true) {
      std::optional<Value> value = TRY(IteratorStepValue(iterated));
      if (!value) {
        bool partial = windows ? allowPartial && !buffer.empty() && double(buffer.size()) < size : !buffer.empty();
        if (!partial) return std::optional<Value>();
        finalYield = true;
        return std::optional<Value>(bufferArray());
      }
      if (windows && double(buffer.size()) == size) buffer.pop_front();
      buffer.push_back(*value);
      if (double(buffer.size()) == size) {
        Value chunk = bufferArray();
        if (!windows) buffer.clear();
        return std::optional<Value>(chunk);
      }
    }
  }

  // At the last, partial Yield the completion is ignored; at the others IfAbruptCloseIterator closes.
  Result<void> resumeWithReturn() override {
    if (finalYield) return {};
    return IteratorClose(iterated, Result<void>());
  }

  Result<void> closeUnderlyingIterators() override { return IteratorClose(iterated, Result<void>()); }

  void trace(Tracer& tracer) override {
    ClosureIteratorHelper::trace(tracer);
    tracer.visit(reinterpret_cast<Cell*>(iterated.iterator));
    tracer.visit(iterated.nextMethod);
    for (const Value& v : buffer) tracer.visit(v);
  }

 private:
  Value bufferArray() {
    ValueVector list;
    for (const Value& v : buffer) list.push_back(v);
    return Value(CreateArrayFromList(list));
  }

  IteratorRecord iterated;
  double size;
  bool windows;
  bool allowPartial;
  bool finalYield = false;
  std::deque<Value> buffer;
};

// The steps both methods share up to GetIteratorDirect: the size must be an integral Number in [1, 2^32 - 1]
// (no coercion); a bad argument closes the receiver.
Result<Value> makeChunking(NativeCall& call, bool windows) {
  Value o = call.thisValue;
  if (!o.isObject()) return ThrowTypeError(windows ? "Iterator.prototype.windows called on non-object" : "Iterator.prototype.chunks called on non-object");
  IteratorRecord iterated{o.asObject(), Value::undefined(), false};
  Value size = call.args[0];
  if (!size.isNumber() || !std::isfinite(size.asNumber()) || std::trunc(size.asNumber()) != size.asNumber()) {
    return closeIteratorWithThrow(iterated, ThrowTypeError(windows ? "windowSize must be an integer" : "chunkSize must be an integer").value);
  }
  double n = size.asNumber();
  if (n < 1 || n > 4294967295.0) {
    return closeIteratorWithThrow(iterated, ThrowRangeError(windows ? "windowSize must be between 1 and 2^32 - 1" : "chunkSize must be between 1 and 2^32 - 1").value);
  }
  bool allowPartial = false;
  if (windows) {
    Value undersized = call.args[1];
    bool onlyFull = undersized.isUndefined() || (undersized.isString() && undersized.asString()->view() == u"only-full");
    allowPartial = undersized.isString() && undersized.asString()->view() == u"allow-partial";
    if (!onlyFull && !allowPartial) {
      return closeIteratorWithThrow(iterated, ThrowTypeError("undersized must be \"only-full\" or \"allow-partial\"").value);
    }
  }
  iterated = TRY(GetIteratorDirect(o.asObject()));
  return Value(make<ChunkingHelper>(iterated, n, windows, allowPartial));
}

Result<Value> IteratorPrototype_chunks(NativeCall& call) { return makeChunking(call, false); }
Result<Value> IteratorPrototype_windows(NativeCall& call) { return makeChunking(call, true); }

void installIteratorChunking(Realm& realm) {
  Object* proto = realm.intrinsic(Intrinsic::IteratorPrototype);
  defineFunction(realm, proto, PropertyKey(u"chunks"), 1, IteratorPrototype_chunks);
  defineFunction(realm, proto, PropertyKey(u"windows"), 1, IteratorPrototype_windows);
}

}  // namespace

SNOWMAN_BUILTIN(IteratorChunking, kOrderLate, installIteratorChunking);

}  // namespace snowman::builtins
