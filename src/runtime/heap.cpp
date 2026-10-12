// The heap: tracks every cell; the collector is allocate-only for now (collect() is a no-op), but the root
// set and tracing are kept correct so it can be switched on. INTERFACES.md section 13.
#include "runtime/heap.h"

#include <algorithm>

namespace snowman {

Heap& heap() {
  static Heap* instance = new Heap();
  return *instance;
}

RootProvider::RootProvider() {
  Heap& h = heap();
  next_ = h.roots_;
  if (next_) next_->prev_ = this;
  h.roots_ = this;
}

RootProvider::RootProvider(const RootProvider&) : RootProvider() {}

RootProvider::~RootProvider() {
  Heap& h = heap();
  if (prev_) prev_->next_ = next_;
  else if (h.roots_ == this) h.roots_ = next_;
  if (next_) next_->prev_ = prev_;
}

void Heap::track(Cell* cell) { cells_.push_back(cell); }

void Heap::collect() {}

void Heap::maybeCollect() {}

void Heap::addStack(const void* low, const void* high) { stacks_.emplace_back(low, high); }

void Heap::removeStack(const void* low) {
  stacks_.erase(std::remove_if(stacks_.begin(), stacks_.end(), [&](const auto& s) { return s.first == low; }),
                stacks_.end());
}

}  // namespace snowman
