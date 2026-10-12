// Memory: every GC thing is a Cell, allocated with make<T>(...). INTERFACES.md section 13.
//
// The collector is precise inside the heap (each cell visits what it holds in trace()) and conservative
// over the C stacks (the main stack and every registered coroutine stack). Anything else that holds cells
// must be a RootProvider (ValueVector, Rooted<T>, the VM, realms). A std::vector<Value> outside a cell is
// NOT a root.
#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace snowman {

class Cell;
class Value;

// Tracer visits the cells a cell refers to while marking.
class Tracer {
 public:
  virtual ~Tracer() = default;
  void visit(Cell* cell) {
    if (cell) visitCell(cell);
  }
  void visit(const Value& value);  // value.h
  template <class Range>
  void visitAll(const Range& range) {
    for (const auto& item : range) visit(item);
  }

 protected:
  virtual void visitCell(Cell* cell) = 0;
};

class Cell {
 public:
  Cell() = default;
  Cell(const Cell&) = delete;
  Cell& operator=(const Cell&) = delete;
  virtual ~Cell() = default;

  // Visit every Value and Cell* this cell holds (including those inside its C++ containers).
  virtual void trace(Tracer&) {}

 private:
  friend class Heap;
  bool marked_ = false;
};

// RootProvider: an off-heap holder of cells. Registers itself on construction, unregisters on destruction.
class RootProvider {
 public:
  RootProvider();
  RootProvider(const RootProvider&);
  RootProvider& operator=(const RootProvider&) { return *this; }
  virtual ~RootProvider();
  virtual void traceRoots(Tracer& tracer) = 0;

 private:
  friend class Heap;
  RootProvider* prev_ = nullptr;
  RootProvider* next_ = nullptr;
};

class Heap {
 public:
  template <class T, class... A>
  T* make(A&&... args) {
    T* cell = new T(std::forward<A>(args)...);
    track(cell);
    return cell;
  }

  // A full collection. Allocate-only in the first version (a no-op).
  void collect();
  // A safepoint: collects if the allocation budget is spent. The interpreter calls it at function entry and
  // loop back-edges; nowhere else may assume cells move or die.
  void maybeCollect();

  // Coroutine stacks the collector scans conservatively while they are suspended or running.
  void addStack(const void* low, const void* high);
  void removeStack(const void* low);

  size_t cellCount() const { return cells_.size(); }

 private:
  friend class RootProvider;
  void track(Cell* cell);

  std::vector<Cell*> cells_;
  std::vector<std::pair<const void*, const void*>> stacks_;
  RootProvider* roots_ = nullptr;
};

Heap& heap();

template <class T, class... A>
T* make(A&&... args) {
  return heap().make<T>(std::forward<A>(args)...);
}

}  // namespace snowman
