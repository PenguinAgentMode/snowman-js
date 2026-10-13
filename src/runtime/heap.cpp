// The heap: tracks every cell. The collector is a mark-sweep that is precise inside the heap (Cell::trace)
// and conservative over the stacks; it stays off (collect() returns at once) until setCollectorEnabled(true).
// INTERFACES.md section 13.
#include "runtime/heap.h"
#include "runtime/vm.h"

#include <atomic>
#include <cstdio>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <cstdlib>
#include <malloc.h>

#include <malloc.h>
#include <pthread.h>

#include <algorithm>
#include <cstdint>

namespace snowman {

namespace {

// A collection frees most of the heap at once. glibc would then return the top of the heap to the system and
// grow it again right after (brk, slow under gVisor): keep freed memory for reuse and grow in larger steps.
// Peak usage is unchanged.
const int kMallocTuned = [] {
  mallopt(M_TRIM_THRESHOLD, 256 << 20);
  mallopt(M_TOP_PAD, 16 << 20);
  return 0;
}();

}  // namespace

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

// The soft memory limit: when the process's resident memory (what the runner's watchdog measures: heap, malloc'd
// buffers, touched stacks) passes it, the engine first gives back what it can (a collection, malloc_trim, the dead
// part of the running stack below the current frame); still over, the next CheckStack throws a catchable
// RangeError("out of memory") instead of the watchdog's kill. Checked after each collection and every 64K cells.
// SNOWMAN_MEM_MB sets it (default 640, under the runner's 700 MB; 0: none). A true malloc failure stays fatal.
size_t memoryLimitBytes() {
  static const size_t limit = [] {
    const char* env = std::getenv("SNOWMAN_MEM_MB");
    long mb = env ? std::atol(env) : 640;
    return mb > 0 ? size_t(mb) << 20 : SIZE_MAX;
  }();
  return limit;
}
std::atomic<bool> gMemoryLimitExceeded{false};
thread_local size_t tAllocationsSinceCheck = 0;

// Resident bytes, /proc/self/statm's second field (matches VmRSS, gVisor included).
size_t residentBytes() {
  int fd = open("/proc/self/statm", O_RDONLY);
  if (fd < 0) return 0;
  char buf[128];
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) return 0;
  buf[n] = 0;
  unsigned long size = 0, resident = 0;
  if (std::sscanf(buf, "%lu %lu", &size, &resident) != 2) return 0;
  return size_t(resident) * size_t(sysconf(_SC_PAGESIZE));
}

// The running stack's pages below the current frame (the stack grows down) hold nothing live: give them back.
void releaseDeadStackBelow(const void* sp) {
  const char* low = vm().stackLimit;
  if (!low) return;
  uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
  uintptr_t begin = (reinterpret_cast<uintptr_t>(low) + page - 1) & ~(page - 1);
  uintptr_t end = (reinterpret_cast<uintptr_t>(sp) - (uintptr_t(256) << 10)) & ~(page - 1);
  if (end > begin) madvise(reinterpret_cast<void*>(begin), end - begin, MADV_DONTNEED);
}

void checkMemoryLimit(Heap& heap, bool collected);

void Heap::track(Cell* cell) {
  cells_.push_back(cell);
  allocatedSinceCollect_++;
  gProcessCells.fetch_add(1, std::memory_order_relaxed);
  // Between collections (far apart with a large live heap) the resident size is looked at every 64K cells.
  if (++tAllocationsSinceCheck >= (size_t(1) << 16)) {
    tAllocationsSinceCheck = 0;
    if (enabled_) checkMemoryLimit(*this, false);
  }
}

// Over the limit: give back free malloc memory and the dead stack, then look again. No collection is forced here: a
// collection at that size allocates its own sorted copy of every cell, which would push the peak further up.
void checkMemoryLimit(Heap&, bool) {
  if (memoryLimitBytes() == SIZE_MAX || residentBytes() <= memoryLimitBytes()) return;
  malloc_trim(0);
  char here;
  releaseDeadStackBelow(&here);
  if (residentBytes() > memoryLimitBytes()) gMemoryLimitExceeded = true;
}

bool TakeHeapLimitExceeded() {
  return gMemoryLimitExceeded.load(std::memory_order_relaxed) && gMemoryLimitExceeded.exchange(false);
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
  checkMemoryLimit(*this, true);

}

}  // namespace snowman
