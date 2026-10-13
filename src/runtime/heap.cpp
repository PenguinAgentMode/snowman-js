// The heap: tracks every cell. The collector is a mark-sweep that is precise inside the heap (Cell::trace)
// and conservative over the stacks; it stays off (collect() returns at once) until setCollectorEnabled(true).
// INTERFACES.md section 13.
#include "runtime/heap.h"

#include <atomic>
#include <cstdlib>
#include <malloc.h>

#include <pthread.h>

#include <algorithm>
#include <cstdint>

namespace snowman {

// One heap per thread (agent); cells never cross threads.
thread_local Heap* tHeap = nullptr;

Heap& heap() {
  if (!tHeap) tHeap = new Heap();
  return *tHeap;
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

// Every thread's ($262.agent) heap together: past this many live cells a heap collects four times as often, so the
// agents' heaps can't each grow to twice their live size at once (the RSS watchdog sees the whole process).
std::atomic<size_t> gProcessCells{0};
constexpr size_t kProcessCellSoftLimit = size_t(3) << 20;

// The soft heap limit: live malloc memory (every thread's, one arena) above it after a collection makes the next
// CheckStack throw a catchable RangeError("out of memory"), well before the runner's RSS watchdog would kill the
// process. SNOWMAN_HEAP_MB sets it (default 600; 0: none). A true malloc failure stays fatal (cli.cpp).
size_t heapLimitBytes() {
  static const size_t limit = [] {
    const char* env = std::getenv("SNOWMAN_HEAP_MB");
    long mb = env ? std::atol(env) : 600;
    return mb > 0 ? size_t(mb) << 20 : SIZE_MAX;
  }();
  return limit;
}
std::atomic<bool> gHeapLimitExceeded{false};
thread_local size_t tAllocationsSinceCheck = 0;

size_t mallocInUse() {
  struct mallinfo2 info = mallinfo2();
  return info.uordblks + info.hblkhd;
}

void Heap::track(Cell* cell) {
  cells_.push_back(cell);
  allocatedSinceCollect_++;
  gProcessCells.fetch_add(1, std::memory_order_relaxed);
  // Between collections (which can be far apart with a large live heap), look at malloc every 64K cells; over the
  // limit, collect now: a live heap still over it then is a RangeError at the next stack check.
  if (++tAllocationsSinceCheck >= (size_t(1) << 16)) {
    tAllocationsSinceCheck = 0;
    if (enabled_ && heapLimitBytes() != SIZE_MAX && mallocInUse() > heapLimitBytes()) collect();
  }
}

bool TakeHeapLimitExceeded() {
  return gHeapLimitExceeded.load(std::memory_order_relaxed) && gHeapLimitExceeded.exchange(false);
}

void Heap::addStack(const void* low, const void* high) { stacks_.emplace_back(low, high); }

void Heap::removeStack(const void* low) {
  stacks_.erase(std::remove_if(stacks_.begin(), stacks_.end(), [&](const auto& s) { return s.first == low; }),
                stacks_.end());
}

namespace {

// Marks with an explicit worklist so deep object graphs do not recurse on the C stack.
class MarkTracer final : public Tracer {
 public:
  std::vector<Cell*> worklist;

 protected:
  void visitCell(Cell* cell) override { worklist.push_back(cell); }
};

}  // namespace

void Heap::scanRange(Tracer& tracer, const void* low, const void* high) {
  auto begin = reinterpret_cast<uintptr_t>(low) & ~uintptr_t(sizeof(void*) - 1);
  auto end = reinterpret_cast<uintptr_t>(high);
  for (uintptr_t p = begin; p + sizeof(void*) <= end; p += sizeof(void*)) {
    Cell* candidate = *reinterpret_cast<Cell* const*>(p);
    if (std::binary_search(sortedCells_.begin(), sortedCells_.end(), candidate)) tracer.visit(candidate);
  }
}

void Heap::maybeCollect() {
  if (!enabled_) return;
  size_t budget = gProcessCells.load(std::memory_order_relaxed) > kProcessCellSoftLimit ? budget_ / 4 : budget_;
  if (allocatedSinceCollect_ >= budget) collect();
}

void Heap::collect() {
  if (!enabled_) return;
  // Spill callee-saved registers into this frame so the stack scan sees them.
  __builtin_unwind_init();
  volatile char marker = 0;
  const void* sp = const_cast<const char*>(&marker);

  sortedCells_ = cells_;
  std::sort(sortedCells_.begin(), sortedCells_.end());
  MarkTracer tracer;

  for (RootProvider* r = roots_; r; r = r->next_) r->traceRoots(tracer);

  bool onRegisteredStack = false;
  for (const auto& [low, high] : stacks_) {
    if (sp >= low && sp < high) {
      onRegisteredStack = true;
      scanRange(tracer, sp, high);
    } else {
      scanRange(tracer, low, high);
    }
  }
  if (!onRegisteredStack) {
    if (!threadStackHigh_) {
      pthread_attr_t attr;
      void* low = nullptr;
      size_t size = 0;
      if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &low, &size);
        pthread_attr_destroy(&attr);
      }
      threadStackLow_ = low;
      threadStackHigh_ = static_cast<const char*>(low) + size;
    }
    scanRange(tracer, sp, threadStackHigh_);
  }

  while (!tracer.worklist.empty()) {
    Cell* cell = tracer.worklist.back();
    tracer.worklist.pop_back();
    if (cell->marked_) continue;
    cell->marked_ = true;
    cell->trace(tracer);
  }

  size_t kept = 0;
  for (Cell* cell : cells_) {
    if (cell->marked_) {
      cell->marked_ = false;
      cells_[kept++] = cell;
    } else {
      delete cell;
    }
  }
  gProcessCells.fetch_sub(cells_.size() - kept, std::memory_order_relaxed);
  cells_.resize(kept);
  sortedCells_.clear();
  sortedCells_.shrink_to_fit();
  allocatedSinceCollect_ = 0;
  budget_ = std::max<size_t>(1 << 20, kept);
  if (heapLimitBytes() != SIZE_MAX && mallocInUse() > heapLimitBytes()) gHeapLimitExceeded = true;
}

}  // namespace snowman
