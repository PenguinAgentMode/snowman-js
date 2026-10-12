// Shared by WeakMap, WeakSet, WeakRef and FinalizationRegistry. Entries are traced strongly for now: the
// collector has no ephemerons yet, so weak targets simply live as long as their holder (allowed: an
// implementation may keep any object alive).
#pragma once

#include <unordered_map>

#include "runtime/object.h"
#include "runtime/vm.h"

namespace snowman::builtins {

// 9.13 CanBeHeldWeakly ( v )
inline bool CanBeHeldWeakly(Value v) {
  if (v.isObject()) return true;
  if (v.isSymbol()) {
    for (auto& [key, symbol] : vm().symbolRegistry) {
      if (symbol == v.asSymbol()) return false;
    }
    return true;
  }
  return false;
}

// Insertion order is unobservable for weak collections, so a hash map by identity is enough.
class WeakCollection : public Object {
 public:
  explicit WeakCollection(Object* prototype) : Object(prototype) {}
  std::unordered_map<Cell*, Value> entries;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    for (auto& [key, value] : entries) {
      tracer.visit(key);
      tracer.visit(value);
    }
  }
};

class WeakMapObject : public WeakCollection {
 public:
  using WeakCollection::WeakCollection;
};

class WeakSetObject : public WeakCollection {
 public:
  using WeakCollection::WeakCollection;
};

}  // namespace snowman::builtins
