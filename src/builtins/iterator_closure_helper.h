// Iterator Helper objects (27.1.2.1) whose closures live outside iterator.cpp: Iterator.zip/zipKeyed
// (iterator_zip.cpp) and Iterator.prototype.chunks/windows (iterator_chunking.cpp). iterator_closure_helper.cpp
// replaces %IteratorHelperPrototype%.next and .return with methods that drive these and pass every other
// receiver to the original methods.
#pragma once

#include <optional>

#include "builtins/util.h"

namespace snowman::builtins {

class ClosureIteratorHelper : public Object {
 public:
  enum class State : uint8_t { SuspendedStart, SuspendedYield, Executing, Completed };
  // With %IteratorHelperPrototype% of the current realm.
  ClosureIteratorHelper();

  // The closure, from the start or the last Yield to the next one: the value yielded, or nullopt when it
  // returns. A throw completes the generator.
  virtual Result<std::optional<Value>> resume() = 0;
  // GeneratorResumeAbrupt with a return completion, suspended at a Yield: the closure's handling of it.
  virtual Result<void> resumeWithReturn() = 0;
  // %IteratorHelperPrototype%.return before the closure ran: IteratorCloseAll([[UnderlyingIterators]]).
  virtual Result<void> closeUnderlyingIterators() = 0;

  State state = State::SuspendedStart;
};

// IteratorCloseAll(iters, completion): IteratorClose each, last first, threading the completion through.
template <typename Iters>
Result<void> IteratorCloseAll(const Iters& iters, Result<void> completion) {
  for (auto it = iters.rbegin(); it != iters.rend(); ++it) completion = IteratorClose(*it, std::move(completion));
  return completion;
}

}  // namespace snowman::builtins
