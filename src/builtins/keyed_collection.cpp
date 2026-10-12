// OrderedTable: the list behind Map and Set.
#include "builtins/keyed_collection.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "runtime/bigint.h"
#include "runtime/operations.h"
#include "runtime/string.h"

namespace snowman::builtins {

size_t ValueHasher::operator()(const Value& v) const {
  uint64_t h;
  switch (v.type()) {
    case ValueType::Number: {
      double d = v.asNumber();
      if (std::isnan(d)) d = NAN;
      if (d == 0) d = 0;
      h = std::bit_cast<uint64_t>(d);
      break;
    }
    case ValueType::String:
      h = v.asString()->hash();
      break;
    case ValueType::BigInt: {
      h = v.asBigInt()->isNegative() ? 0x9E37 : 0;
      for (uint32_t limb : v.asBigInt()->magnitude()) h = h * 1000003 ^ limb;
      break;
    }
    case ValueType::Boolean:
      h = v.asBoolean() ? 3 : 2;
      break;
    case ValueType::Symbol:
    case ValueType::Object:
      h = reinterpret_cast<uintptr_t>(v.asCell()) >> 3;
      break;
    default:
      h = uint64_t(v.type());
      break;
  }
  h *= 0x9E3779B97F4A7C15ull;
  return size_t(h ^ (h >> 31));
}

bool SameValueZeroEqual::operator()(const Value& a, const Value& b) const { return SameValueZero(a, b); }

Value CanonicalizeKeyedCollectionKey(Value key) {
  if (key.isNumber() && key.asNumber() == 0) return Value::number(0);
  return key;
}

long OrderedTable::find(Value key) const {
  auto it = index_.find(key);
  return it == index_.end() ? -1 : long(it->second);
}

Value OrderedTable::get(Value key) const {
  long i = find(key);
  return i < 0 ? Value::undefined() : entries_[size_t(i)].value;
}

void OrderedTable::set(Value key, Value value) {
  long i = find(key);
  if (i >= 0) {
    entries_[size_t(i)].value = value;
    return;
  }
  index_.emplace(key, entries_.size());
  entries_.push_back({key, value});
  live_++;
}

bool OrderedTable::remove(Value key) {
  auto it = index_.find(key);
  if (it == index_.end()) return false;
  Entry& e = entries_[it->second];
  e.key = Value::empty();
  e.value = Value::empty();
  index_.erase(it);
  live_--;
  maybeCompact();
  return true;
}

void OrderedTable::clear() {
  for (Entry& e : entries_) {
    e.key = Value::empty();
    e.value = Value::empty();
  }
  index_.clear();
  live_ = 0;
  maybeCompact();
}

void OrderedTable::maybeCompact() {
  size_t holes = entries_.size() - live_;
  if (holes < 16 || holes < live_) return;
  std::vector<size_t> removed;
  std::vector<Entry> kept;
  kept.reserve(live_);
  for (size_t i = 0; i < entries_.size(); i++) {
    if (entries_[i].key.isEmpty()) removed.push_back(i);
    else kept.push_back(entries_[i]);
  }
  entries_ = std::move(kept);
  index_.clear();
  for (size_t i = 0; i < entries_.size(); i++) index_.emplace(entries_[i].key, i);
  compactions_.push_back(std::move(removed));
  generation_++;
}

size_t OrderedTable::adjust(Position& position) const {
  while (position.generation < generation_) {
    const std::vector<size_t>& removed = compactions_[position.generation];
    size_t before = size_t(std::lower_bound(removed.begin(), removed.end(), position.index) - removed.begin());
    position.index -= before;
    position.generation++;
  }
  return position.index;
}

void OrderedTable::trace(Tracer& tracer) const {
  for (const Entry& e : entries_) {
    tracer.visit(e.key);
    tracer.visit(e.value);
  }
}

}  // namespace snowman::builtins
