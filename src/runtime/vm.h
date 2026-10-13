// The agent: the execution context stack, well-known symbols, the job queue. Single-threaded. INTERFACES.md
// sections 8 and 12.
#pragma once

#include <deque>
#include <string>
#include <vector>

#include "runtime/realm.h"
#include "runtime/symbol.h"

namespace snowman {

// The runtime's part of an execution context. The interpreter derives its script/function/module contexts
// from it; built-in functions push a plain one with their realm.
struct ExecutionContext {
  Realm* realm = nullptr;
  Object* function = nullptr;  // nullptr for script and module contexts
  ExecutionContext* previous = nullptr;
  virtual ~ExecutionContext() = default;
};

// A pending job (HostEnqueuePromiseJob, HostEnqueueGenericJob). run() is called with no context running.
class Job : public Cell {
 public:
  explicit Job(Realm* realm) : realm(realm) {}
  virtual Result<void> run() = 0;
  void trace(Tracer& tracer) override { tracer.visit(reinterpret_cast<Cell*>(realm)); }
  Realm* realm;  // nullptr when the job has no realm
};

class VM : public RootProvider {
 public:
  WellKnownSymbols symbols;

  // The running execution context and its realm.
  ExecutionContext* running = nullptr;
  Realm* currentRealm() const { return running ? running->realm : initialRealm; }
  void pushContext(ExecutionContext* context) {
    context->previous = running;
    running = context;
  }
  void popContext() { running = running->previous; }
  // The realm used when no execution context is running (set by the CLI after creating the first realm).
  Realm* initialRealm = nullptr;

  // Every realm ever created (roots).
  std::vector<Realm*> realms;
  // The GlobalSymbolRegistry (Symbol.for / Symbol.keyFor): shared by all realms.
  std::vector<std::pair<String*, Symbol*>> symbolRegistry;

  // HostEnqueuePromiseJob. runJobs() drains the queue in FIFO order (jobs may enqueue more); it returns the
  // first job's throw, if any, after which the queue is left as it is.
  void enqueueJob(Job* job) { jobs_.push_back(job); }
  Result<void> runJobs();
  bool hasJobs() const { return !jobs_.empty(); }

  // The lowest usable address of the stack the running code is on; CheckStack() throws a RangeError near it.
  // Whoever switches stacks (coroutines, the CLI's main stack) keeps it current.
  const char* stackLimit = nullptr;
  // The agent's [[CanBlock]] (host-defined; the CLI's --can-block=false): Atomics.wait throws when false.
  bool canBlock = true;

  void traceRoots(Tracer& tracer) override;

 private:
  std::deque<Job*> jobs_;
};

VM& vm();

// Throws a RangeError ("Maximum call stack size exceeded") when the C stack is nearly exhausted. Call it
// on every path that can recurse without bound (function calls, Proxy traps, JSON, toString chains).
Result<void> CheckStack();

}  // namespace snowman
