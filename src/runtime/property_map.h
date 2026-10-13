// PropertyMap: an object's own-property storage. Integer indices live in a dense vector (data properties
// that are writable, enumerable and configurable) or a sorted sparse map; other keys keep insertion order.
// keys() yields the OrdinaryOwnPropertyKeys order: indices ascending, then strings, then symbols, each in
// creation order. INTERFACES.md section 5.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "runtime/property.h"

namespace snowman {

// One own property. For an accessor, value is [[Get]] and setter is [[Set]] (undefined when absent).
struct PropertySlot {
  Value value;
  Value setter;
  Attr attrs = Attr::None;
  bool accessor = false;

  bool writable() const { return has(attrs, Attr::Writable); }
  bool enumerable() const { return has(attrs, Attr::Enumerable); }
  bool configurable() const { return has(attrs, Attr::Configurable); }
  PropertyDescriptor toDescriptor() const;
  static PropertySlot data(Value value, Attr attrs) { return {value, Value(), attrs, false}; }
  static PropertySlot accessorPair(Value getter, Value setter, Attr attrs) { return {getter, setter, attrs, true}; }
};

class PropertyMap {
 public:
  // Copy of the property, or nullopt.
  std::optional<PropertySlot> get(const PropertyKey& key) const;
  bool contains(const PropertyKey& key) const;
  // Adds the property (at the end of its group) or replaces it in place.
  void put(const PropertyKey& key, const PropertySlot& slot);
  // Replaces the [[Value]] of an existing data property; false if there is none.
  bool setValue(const PropertyKey& key, Value value);
  // Same, only for a writable data property.
  bool setWritableValue(const PropertyKey& key, Value value);
  // Removes the property; false if there was none.
  bool remove(const PropertyKey& key);
  size_t size() const;

  // Own keys in OrdinaryOwnPropertyKeys order.
  std::vector<PropertyKey> keys() const;
  // Calls f(key, slot) for every property in key order.
  template <class F>
  void forEach(F&& f) const {
    for (const PropertyKey& key : keys()) f(key, *get(key));
  }

  // The integer-index keys, ascending.
  std::vector<uint32_t> indexKeys() const;
  // The smallest index key ≥ from / the largest index key ≤ from, if any.
  std::optional<uint32_t> nextIndex(uint32_t from) const;
  std::optional<uint32_t> prevIndex(uint32_t from) const;
  // The largest index key + 1 (0 when there is none).
  uint64_t indexEnd() const;
  // Deletes the index properties ≥ from, highest first, stopping at a non-configurable one. Returns the
  // index of the property that could not be deleted, or nullopt when all were.
  std::optional<uint32_t> truncateIndices(uint32_t from);

  // Fast element access for callers that know the layout (arrays).
  const std::vector<Value>& dense() const { return dense_; }

  void trace(Tracer& tracer) const;

 private:
  struct Entry {
    PropertyKey key;
    PropertySlot slot;
    bool deleted;
  };
  int findNamed(const PropertyKey& key) const;
  void compact();
  void rebuildTable();
  void tableInsert(int32_t index);

  std::vector<Value> dense_;  // empty() = hole
  size_t denseCount_ = 0;
  std::map<uint32_t, PropertySlot> sparse_;
  std::vector<Entry> named_;
  size_t namedDeleted_ = 0;
  // Open-addressing index into named_ once it is large: -1 empty, -2 removed; power-of-two size.
  std::vector<int32_t> table_;
  size_t tableUsed_ = 0;
};

}  // namespace snowman
