// PropertyMap implementation.
#include "runtime/property_map.h"

#include <algorithm>

namespace snowman {

namespace {
constexpr size_t kIndexThreshold = 8;
constexpr uint32_t kMaxDenseGap = 64;
constexpr uint32_t kMaxDense = 1u << 26;
}  // namespace

PropertyDescriptor PropertySlot::toDescriptor() const {
  PropertyDescriptor d;
  if (accessor) {
    d.get = value;
    d.set = setter;
  } else {
    d.value = value;
    d.writable = writable();
  }
  d.enumerable = enumerable();
  d.configurable = configurable();
  return d;
}

int PropertyMap::findNamed(const PropertyKey& key) const {
  if (!namedIndex_.empty()) {
    auto it = namedIndex_.find(key);
    return it == namedIndex_.end() ? -1 : int(it->second);
  }
  for (size_t i = 0; i < named_.size(); i++) {
    if (!named_[i].deleted && named_[i].key == key) return int(i);
  }
  return -1;
}

std::optional<PropertySlot> PropertyMap::get(const PropertyKey& key) const {
  if (key.isIndex()) {
    uint32_t i = key.asIndex();
    if (i < dense_.size() && !dense_[i].isEmpty()) return PropertySlot::data(dense_[i], Attr::All);
    auto it = sparse_.find(i);
    if (it == sparse_.end()) return std::nullopt;
    return it->second;
  }
  int i = findNamed(key);
  if (i < 0) return std::nullopt;
  return named_[i].slot;
}

bool PropertyMap::contains(const PropertyKey& key) const {
  if (key.isIndex()) {
    uint32_t i = key.asIndex();
    if (i < dense_.size() && !dense_[i].isEmpty()) return true;
    return sparse_.count(i) != 0;
  }
  return findNamed(key) >= 0;
}

void PropertyMap::put(const PropertyKey& key, const PropertySlot& slot) {
  if (key.isIndex()) {
    uint32_t i = key.asIndex();
    bool plain = !slot.accessor && slot.attrs == Attr::All && !slot.value.isEmpty();
    if (i < dense_.size() && !dense_[i].isEmpty()) {
      if (plain) {
        dense_[i] = slot.value;
        return;
      }
      dense_[i] = Value::empty();
      denseCount_--;
      sparse_[i] = slot;
      return;
    }
    auto it = sparse_.find(i);
    if (it != sparse_.end()) {
      it->second = slot;
      return;
    }
    if (plain && i < kMaxDense && i <= dense_.size() + kMaxDenseGap) {
      if (i >= dense_.size()) dense_.resize(size_t(i) + 1, Value::empty());
      dense_[i] = slot.value;
      denseCount_++;
      return;
    }
    sparse_[i] = slot;
    return;
  }
  int i = findNamed(key);
  if (i >= 0) {
    named_[i].slot = slot;
    return;
  }
  named_.push_back({key, slot, false});
  if (!namedIndex_.empty()) {
    namedIndex_[key] = uint32_t(named_.size() - 1);
  } else if (named_.size() - namedDeleted_ > kIndexThreshold) {
    for (size_t k = 0; k < named_.size(); k++) {
      if (!named_[k].deleted) namedIndex_[named_[k].key] = uint32_t(k);
    }
  }
}

bool PropertyMap::setValue(const PropertyKey& key, Value value) {
  if (key.isIndex()) {
    uint32_t i = key.asIndex();
    if (i < dense_.size() && !dense_[i].isEmpty()) {
      dense_[i] = value;
      return true;
    }
    auto it = sparse_.find(i);
    if (it == sparse_.end() || it->second.accessor) return false;
    it->second.value = value;
    return true;
  }
  int i = findNamed(key);
  if (i < 0 || named_[i].slot.accessor) return false;
  named_[i].slot.value = value;
  return true;
}

bool PropertyMap::setWritableValue(const PropertyKey& key, Value value) {
  if (key.isIndex()) {
    uint32_t i = key.asIndex();
    if (i < dense_.size() && !dense_[i].isEmpty()) {
      dense_[i] = value;
      return true;
    }
    auto it = sparse_.find(i);
    if (it == sparse_.end() || it->second.accessor || !it->second.writable()) return false;
    it->second.value = value;
    return true;
  }
  int i = findNamed(key);
  if (i < 0 || named_[i].slot.accessor || !named_[i].slot.writable()) return false;
  named_[i].slot.value = value;
  return true;
}

bool PropertyMap::remove(const PropertyKey& key) {
  if (key.isIndex()) {
    uint32_t i = key.asIndex();
    if (i < dense_.size() && !dense_[i].isEmpty()) {
      dense_[i] = Value::empty();
      denseCount_--;
      while (!dense_.empty() && dense_.back().isEmpty()) dense_.pop_back();
      return true;
    }
    return sparse_.erase(i) != 0;
  }
  int i = findNamed(key);
  if (i < 0) return false;
  named_[i].deleted = true;
  named_[i].slot = PropertySlot();
  namedDeleted_++;
  if (!namedIndex_.empty()) namedIndex_.erase(key);
  if (namedDeleted_ > 16 && namedDeleted_ * 2 > named_.size()) compact();
  return true;
}

void PropertyMap::compact() {
  std::vector<Entry> live;
  live.reserve(named_.size() - namedDeleted_);
  for (auto& e : named_) {
    if (!e.deleted) live.push_back(e);
  }
  named_ = std::move(live);
  namedDeleted_ = 0;
  namedIndex_.clear();
  if (named_.size() > kIndexThreshold) {
    for (size_t k = 0; k < named_.size(); k++) namedIndex_[named_[k].key] = uint32_t(k);
  }
}

size_t PropertyMap::size() const { return denseCount_ + sparse_.size() + named_.size() - namedDeleted_; }

std::vector<uint32_t> PropertyMap::indexKeys() const {
  std::vector<uint32_t> out;
  out.reserve(denseCount_ + sparse_.size());
  auto it = sparse_.begin();
  for (uint32_t i = 0; i < dense_.size(); i++) {
    if (dense_[i].isEmpty()) continue;
    while (it != sparse_.end() && it->first < i) out.push_back((it++)->first);
    out.push_back(i);
  }
  while (it != sparse_.end()) out.push_back((it++)->first);
  return out;
}

std::optional<uint32_t> PropertyMap::nextIndex(uint32_t from) const {
  std::optional<uint32_t> best;
  for (size_t i = from; i < dense_.size(); i++) {
    if (!dense_[i].isEmpty()) {
      best = uint32_t(i);
      break;
    }
  }
  auto it = sparse_.lower_bound(from);
  if (it != sparse_.end() && (!best || it->first < *best)) best = it->first;
  return best;
}

std::optional<uint32_t> PropertyMap::prevIndex(uint32_t from) const {
  std::optional<uint32_t> best;
  if (!dense_.empty()) {
    for (size_t i = std::min<size_t>(from, dense_.size() - 1) + 1; i-- > 0;) {
      if (!dense_[i].isEmpty()) {
        best = uint32_t(i);
        break;
      }
    }
  }
  auto it = sparse_.upper_bound(from);
  if (it != sparse_.begin()) {
    --it;
    if (!best || it->first > *best) best = it->first;
  }
  return best;
}

uint64_t PropertyMap::indexEnd() const {
  uint64_t end = dense_.size();
  if (!sparse_.empty()) end = std::max<uint64_t>(end, uint64_t(sparse_.rbegin()->first) + 1);
  return end;
}

std::vector<PropertyKey> PropertyMap::keys() const {
  std::vector<PropertyKey> out;
  out.reserve(size());
  for (uint32_t i : indexKeys()) out.push_back(PropertyKey::index(i));
  for (auto& e : named_) {
    if (!e.deleted && e.key.isString()) out.push_back(e.key);
  }
  for (auto& e : named_) {
    if (!e.deleted && e.key.isSymbol()) out.push_back(e.key);
  }
  return out;
}

std::optional<uint32_t> PropertyMap::truncateIndices(uint32_t from) {
  // The highest non-configurable index ≥ from bounds what can be deleted.
  std::optional<uint32_t> blocker;
  for (auto it = sparse_.rbegin(); it != sparse_.rend() && it->first >= from; ++it) {
    if (!it->second.configurable()) {
      blocker = it->first;
      break;
    }
  }
  uint64_t keepBelow = blocker ? uint64_t(*blocker) + 1 : from;
  sparse_.erase(sparse_.lower_bound(uint32_t(keepBelow)), sparse_.end());
  if (dense_.size() > keepBelow) {
    for (size_t i = keepBelow; i < dense_.size(); i++) {
      if (!dense_[i].isEmpty()) denseCount_--;
    }
    dense_.resize(keepBelow);
  }
  while (!dense_.empty() && dense_.back().isEmpty()) dense_.pop_back();
  return blocker;
}

void PropertyMap::trace(Tracer& tracer) const {
  tracer.visitAll(dense_);
  for (auto& [index, slot] : sparse_) {
    tracer.visit(slot.value);
    tracer.visit(slot.setter);
  }
  for (auto& e : named_) {
    e.key.trace(tracer);
    tracer.visit(e.slot.value);
    tracer.visit(e.slot.setter);
  }
}

}  // namespace snowman
