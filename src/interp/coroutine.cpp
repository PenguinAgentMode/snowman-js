// Stackful coroutines. See coroutine.h.
//
// The collector scans coroutine stacks conservatively, so each one is registered with the heap: the running
// coroutine with its whole range (the collector scans the stack it runs on from the stack pointer up), a
// suspended one (or the thread's own stack while a coroutine runs) from its saved stack pointer up.
// __builtin_unwind_init() spills the callee-saved registers before every switch, so pointers held in
// registers are on a scanned stack too.
#include "interp/coroutine.h"

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "runtime/heap.h"
#include "runtime/vm.h"

namespace snowman::interp {

namespace {

Coroutine* gCurrent = nullptr;
// Finished default-sized stacks, kept for reuse: async code makes and finishes coroutines constantly.
std::vector<char*> gStackPool;
constexpr size_t kMaxPooledStacks = 64;

size_t pageSize() {
  static const size_t size = size_t(sysconf(_SC_PAGESIZE));
  return size;
}

// The high end of the thread's own stack.
const void* threadStackHigh() {
  static const void* high = [] {
    pthread_attr_t attr;
    void* low = nullptr;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
      pthread_attr_getstack(&attr, &low, &size);
      pthread_attr_destroy(&attr);
    }
    return static_cast<const void*>(static_cast<char*>(low) + size);
  }();
  return high;
}

// An address below the caller's frame: everything the caller has spilled lies above it.
[[gnu::noinline]] const void* approximateStackPointer() { return __builtin_frame_address(0); }

}  // namespace

Coroutine::Coroutine(Entry entry, void* data, size_t stackSize) : entry_(entry), data_(data) {
  size_t page = pageSize();
  stackSize_ = (stackSize + page - 1) / page * page;
}

Coroutine::~Coroutine() {
  // A suspended coroutine's frames may hold root registrations: leave its stack mapped and registered.
  if (finished_ || !started_) releaseStack();
}

Coroutine* Coroutine::current() { return gCurrent; }

void Coroutine::releaseStack() {
  if (!mapping_) return;
  if (registeredLow_) heap().removeStack(registeredLow_);
  registeredLow_ = nullptr;
  if (stackSize_ == kDefaultStackSize && gStackPool.size() < kMaxPooledStacks) {
    gStackPool.push_back(mapping_);
  } else {
    munmap(mapping_, stackSize_ + pageSize());
  }
  mapping_ = stackLow_ = stackHigh_ = nullptr;
}

void Coroutine::trampoline(unsigned high, unsigned low) {
  auto* self = reinterpret_cast<Coroutine*>((uintptr_t(high) << 32) | uintptr_t(low));
  self->entry_(self, self->data_);
  self->finished_ = true;
  heap().removeStack(self->registeredLow_);
  self->registeredLow_ = nullptr;
  setcontext(&self->resumer_);
  std::abort();  // unreachable
}

void Coroutine::resume() {
  if (running_ || finished_) std::abort();
  if (!started_) {
    size_t guard = pageSize();
    if (stackSize_ == kDefaultStackSize && !gStackPool.empty()) {
      mapping_ = gStackPool.back();
      gStackPool.pop_back();
    } else {
      void* mapping = mmap(nullptr, stackSize_ + guard, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
      if (mapping == MAP_FAILED) {
        std::fputs("fatal: cannot map a coroutine stack\n", stdout);
        std::exit(70);
      }
      mapping_ = static_cast<char*>(mapping);
      mprotect(mapping_, guard, PROT_NONE);
    }
    stackLow_ = mapping_ + guard;
    stackHigh_ = stackLow_ + stackSize_;
    getcontext(&self_);
    self_.uc_stack.ss_sp = stackLow_;
    self_.uc_stack.ss_size = stackSize_;
    self_.uc_link = nullptr;
    auto address = reinterpret_cast<uintptr_t>(this);
    makecontext(&self_, reinterpret_cast<void (*)()>(&Coroutine::trampoline), 2, unsigned(address >> 32),
                unsigned(address & 0xFFFFFFFFu));
    started_ = true;
  }

  __builtin_unwind_init();
  // The resumer's stack stays live from here up while the coroutine runs.
  Coroutine* resumer = gCurrent;
  const void* resumerSp = approximateStackPointer();
  if (resumer) {
    heap().removeStack(resumer->registeredLow_);
    resumer->registeredLow_ = resumerSp;
    heap().addStack(resumerSp, resumer->stackHigh_);
  } else {
    heap().addStack(resumerSp, threadStackHigh());
  }
  if (registeredLow_) heap().removeStack(registeredLow_);
  registeredLow_ = stackLow_;
  heap().addStack(stackLow_, stackHigh_);

  resumerCoroutine_ = resumer;
  resumerStackLimit_ = vm().stackLimit;
  // CheckStack() throws below the limit: leave room for the error path and for code between checks.
  vm().stackLimit = stackLow_ + (stackSize_ >= (size_t(64) << 20) ? (size_t(1) << 20) : (size_t(128) << 10));
  gCurrent = this;
  running_ = true;

  swapcontext(&resumer_, &self_);

  running_ = false;
  gCurrent = resumerCoroutine_;
  vm().stackLimit = resumerStackLimit_;
  heap().removeStack(resumerSp);
  if (resumer) {
    resumer->registeredLow_ = resumer->stackLow_;
    heap().addStack(resumer->stackLow_, resumer->stackHigh_);
  }
  if (finished_) releaseStack();
}

void Coroutine::suspend() {
  if (!running_) std::abort();
  __builtin_unwind_init();
  const void* sp = approximateStackPointer();
  heap().removeStack(registeredLow_);
  registeredLow_ = sp;
  heap().addStack(sp, stackHigh_);
  swapcontext(&self_, &resumer_);
  // resume() registered the whole stack again.
}

}  // namespace snowman::interp
