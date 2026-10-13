// Stackful coroutines. See coroutine.h.
//
// The collector scans coroutine stacks conservatively, so each one is registered with the heap: the running
// coroutine with its whole range (the collector scans the stack it runs on from the stack pointer up), a
// suspended one (or the thread's own stack while a coroutine runs) from its saved stack pointer up.
// The switch pushes the callee-saved registers on the stack it leaves, so pointers held in registers are on
// a scanned stack too.
#include "interp/coroutine.h"

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "runtime/heap.h"
#include "runtime/vm.h"

// snowman_coroutine_switch(save, next): push the callee-saved registers, store the stack pointer in *save,
// switch to the stack `next` and pop its registers. A new stack starts in snowman_coroutine_start with the
// coroutine in rbx.
extern "C" void snowman_coroutine_switch(void** save, void* next);
extern "C" void snowman_coroutine_start();
asm(R"(
  .text
  .globl snowman_coroutine_switch
  .type snowman_coroutine_switch, @function
snowman_coroutine_switch:
  pushq %rbp
  pushq %rbx
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  movq %rsp, (%rdi)
  movq %rsi, %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rbx
  popq %rbp
  ret
  .size snowman_coroutine_switch, .-snowman_coroutine_switch
  .globl snowman_coroutine_start
  .type snowman_coroutine_start, @function
snowman_coroutine_start:
  movq %rbx, %rdi
  call snowman_coroutine_main
  ud2
  .size snowman_coroutine_start, .-snowman_coroutine_start
)");

extern "C" [[noreturn]] void snowman_coroutine_main(void* self) {
  snowman::interp::Coroutine::trampoline(static_cast<snowman::interp::Coroutine*>(self));
}

namespace snowman::interp {

namespace {

Coroutine* gCurrent = nullptr;
// Finished default-sized stacks, kept for reuse: async code makes and finishes coroutines constantly.
std::vector<char*> gStackPool;
constexpr size_t kMaxPooledStacks = 64;
// The bytes of the stacks started and not yet released (pooled ones aside), against kStackBytesBudget.
size_t gStackBytes = 0;

// CheckStack() throws below a stack's limit: leave room under it for the error path and for code between checks.
size_t stackMargin(size_t stackSize) { return stackSize >= (size_t(64) << 20) ? size_t(1) << 20 : size_t(64) << 10; }
// The main stack's usable part: the budget for the running coroutines together.
constexpr size_t kNestedBudget = Coroutine::kMainStackSize - (size_t(1) << 20);
// Less room than this left for a coroutine and it is not resumed.
constexpr size_t kMinRoom = size_t(16) << 10;

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

bool Coroutine::mapStack() {
  if (gStackBytes + stackSize_ > kStackBytesBudget) return false;
  size_t guard = pageSize();
  if (stackSize_ == kDefaultStackSize && !gStackPool.empty()) {
    mapping_ = gStackPool.back();
    gStackPool.pop_back();
  } else {
    void* mapping = mmap(nullptr, stackSize_ + guard, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (mapping == MAP_FAILED) return false;
    mapping_ = static_cast<char*>(mapping);
    mprotect(mapping_, guard, PROT_NONE);
  }
  gStackBytes += stackSize_;
  stackLow_ = mapping_ + guard;
  stackHigh_ = stackLow_ + stackSize_;
  return true;
}

void Coroutine::releaseStack() {
  if (!mapping_) return;
  if (registeredLow_) heap().removeStack(registeredLow_);
  registeredLow_ = nullptr;
  gStackBytes -= stackSize_;
  if (stackSize_ == kDefaultStackSize && gStackPool.size() < kMaxPooledStacks) {
    gStackPool.push_back(mapping_);
  } else {
    munmap(mapping_, stackSize_ + pageSize());
  }
  mapping_ = stackLow_ = stackHigh_ = nullptr;
}

void Coroutine::trampoline(Coroutine* self) {
  self->entry_(self, self->data_);
  self->finished_ = true;
  heap().removeStack(self->registeredLow_);
  self->registeredLow_ = nullptr;
  snowman_coroutine_switch(&self->selfSp_, self->resumerSp_);
  std::abort();  // never resumed
}

bool Coroutine::resume() {
  if (running_ || finished_) std::abort();
  Coroutine* resumer = gCurrent;
  // Below the registers the switch pushes on the resumer's stack.
  const void* resumerSp = static_cast<const char*>(approximateStackPointer()) - 128;
  // The running coroutines below this one, and this one's own suspended frames, against the nested budget.
  size_t below = resumer ? resumer->usedBelow_ + size_t(resumer->stackHigh_ - static_cast<const char*>(resumerSp)) : 0;
  size_t own = started_ ? size_t(stackHigh_ - static_cast<const char*>(selfSp_)) : 0;
  if (resumer && below + own + kMinRoom > kNestedBudget) return false;
  if (!started_) {
    if (!mapStack()) return false;
    // The frame snowman_coroutine_switch pops: r15 r14 r13 r12 rbx(this) rbp, then the return address.
    // snowman_coroutine_start's call then runs with a 16-byte aligned stack.
    auto** frame = reinterpret_cast<void**>(stackHigh_ - 72);
    for (int i = 0; i < 6; i++) frame[i] = nullptr;
    frame[4] = this;
    frame[6] = reinterpret_cast<void*>(&snowman_coroutine_start);
    selfSp_ = frame;
    started_ = true;
  }

  // The resumer's stack stays live from here up while the coroutine runs.
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
  usedBelow_ = below;
  const char* limit = stackLow_ + stackMargin(stackSize_);
  // A nested coroutine may use what the ones below it leave of the budget, if that is less than its stack.
  if (resumer && size_t(stackHigh_ - limit) > kNestedBudget - below) limit = stackHigh_ - (kNestedBudget - below);
  vm().stackLimit = limit;
  gCurrent = this;
  running_ = true;

  snowman_coroutine_switch(&resumerSp_, selfSp_);

  running_ = false;
  gCurrent = resumerCoroutine_;
  vm().stackLimit = resumerStackLimit_;
  heap().removeStack(resumerSp);
  if (resumer) {
    resumer->registeredLow_ = resumer->stackLow_;
    heap().addStack(resumer->stackLow_, resumer->stackHigh_);
  }
  if (finished_) releaseStack();
  return true;
}

void Coroutine::suspend() {
  if (!running_) std::abort();
  const void* sp = approximateStackPointer();
  heap().removeStack(registeredLow_);
  registeredLow_ = static_cast<const char*>(sp) - 128;  // below the registers the switch pushes
  heap().addStack(registeredLow_, stackHigh_);
  snowman_coroutine_switch(&selfSp_, resumerSp_);
  // resume() registered the whole stack again.
}

}  // namespace snowman::interp
