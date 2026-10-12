// The [[MapData]] / [[SetData]] list of Map and Set (24.1, 24.2): insertion-ordered entries with a
// SameValueZero hash index. Removing an entry leaves a hole, as in the spec; holes are compacted away
// later, and every compaction is logged so that an index held across a compaction (by an iterator or by
// a forEach loop that calls out to JS) can be translated with adjust().
#pragma once

#include <unordered_map>
#include <vector>

#include "runtime/object.h"

namespace snowman::builtins {

struct ValueHasher {
  size_t operator()(const Value& v) const;
};
struct SameValueZeroEqual {
  bool operator()(const Value& a, const Value& b) const;
};

// 24.5.1 CanonicalizeKeyedCollectionKey: -0 becomes +0.
Value CanonicalizeKeyedCollectionKey(Value key);

class OrderedTable {
 public:
  struct Entry {
    Value key;  // empty once removed
    Value value;
  };
  // A position in the list that stays meaningful across compactions.
  struct Position {
    size_t generation = 0;
    size_t index = 0;
  };

  size_t liveCount() const { return live_; }
  const std::vector<Entry>& entries() const { return entries_; }
  // -1 when absent. key must be canonicalized.
  long find(Value key) const;
  bool has(Value key) const { return find(key) >= 0; }
  Value get(Value key) const;
  void set(Value key, Value value);
  bool remove(Value key);
  void clear();
  Position begin() const { return {generation_, 0}; }
  // Brings `position` up to date with the compactions since it was taken; returns its index.
  size_t adjust(Position& position) const;
  void trace(Tracer& tracer) const;

 private:
  void maybeCompact();
  std::vector<Entry> entries_;
  std::unordered_map<Value, size_t, ValueHasher, SameValueZeroEqual> index_;
  size_t live_ = 0;
  size_t generation_ = 0;
  // compactions_[g]: the (sorted) indices removed by the compaction that ended generation g.
  std::vector<std::vector<size_t>> compactions_;
};

class MapObject : public Object {
 public:
  explicit MapObject(Object* prototype) : Object(prototype) {}
  OrderedTable mapData;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    mapData.trace(tracer);
  }
};

class SetObject : public Object {
 public:
  explicit SetObject(Object* prototype) : Object(prototype) {}
  OrderedTable setData;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    setData.trace(tracer);
  }
};

}  // namespace snowman::builtins
