// Stackful coroutines (a small x86-64 register switch, mmap'd stacks with a guard page). Generators and async functions run their
// bodies on one, so the evaluator stays a plain recursive function: yield and await are a switch back to the
// resumer. The CLI also runs the whole program on a large one, so CheckStack() can turn deep recursion into a
// RangeError. INTERFACES.md section 12.
//
// Memory: under gVisor a started stack commits its whole (small) mapping on first touch, so every started,
// unfinished coroutine costs its full stack size. Running coroutines nest (a resumer waits on its own stack),
// and together they count as one stack against kMainStackSize: a resume deeper than that is refused, as is a
// start past kStackBytesBudget or when the stack can't be mapped. The caller reports a refusal as a RangeError.
#pragma once

#include <cstddef>

namespace snowman::interp {

class Coroutine {
 public:
  using Entry = void (*)(Coroutine* self, void* data);

  // Generators and async functions: about 70 plain calls deep (~2.75 KB each) above the CheckStack margin.
  static constexpr size_t kDefaultStackSize = size_t(256) << 10;
  // The program itself (MAP_NORESERVE: only what deep recursion touches is memory). It holds the
  // tail-call-optimization tests' 100,000 calls (300-505 MB at 3-5 KB a call); it can shrink once the
  // interpreter has proper tail calls.
  static constexpr size_t kMainStackSize = size_t(480) << 20;
  // Every stack mapped at once (the main one included): the main stack plus 1,024 default stacks.
  static constexpr size_t kStackBytesBudget = kMainStackSize + (size_t(256) << 20);

  // Nothing runs until the first resume().
  Coroutine(Entry entry, void* data, size_t stackSize = kDefaultStackSize);
  // A coroutine is destroyed only when finished or never started: a suspended one still has live frames.
  ~Coroutine();
  Coroutine(const Coroutine&) = delete;
  Coroutine& operator=(const Coroutine&) = delete;

  // Switches to the coroutine until it suspends or its entry returns. Not while it is running. False, with
  // nothing run, when there is no stack for it (see the top of this file).
  [[nodiscard]] bool resume();
  // Inside the coroutine: switches back to whoever resumed it.
  void suspend();

  bool started() const { return started_; }
  bool finished() const { return finished_; }
  bool running() const { return running_; }

  // The innermost running coroutine (nullptr on the thread's own stack).
  static Coroutine* current();

 public:
  // Where a new coroutine's stack starts (internal to coroutine.cpp).
  [[noreturn]] static void trampoline(Coroutine* self);

 private:
  bool mapStack();
  void releaseStack();

  Entry entry_;
  void* data_;
  size_t stackSize_;
  char* mapping_ = nullptr;  // guard page, then the usable stack [stackLow_, stackHigh_)
  char* stackLow_ = nullptr;
  char* stackHigh_ = nullptr;
  const void* registeredLow_ = nullptr;  // the range currently registered with the heap starts here
  void* selfSp_ = nullptr;     // the coroutine's saved stack pointer while it is not running
  void* resumerSp_ = nullptr;  // the resumer's while the coroutine runs
  const char* resumerStackLimit_ = nullptr;
  Coroutine* resumerCoroutine_ = nullptr;
  size_t usedBelow_ = 0;  // while running: the stack the running coroutines below this one use
  bool started_ = false;
  bool finished_ = false;
  bool running_ = false;
};

}  // namespace snowman::interp
