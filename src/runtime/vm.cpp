// The VM: well-known symbols, the job queue, the stack check.
#include "runtime/vm.h"

#include <pthread.h>


#include "runtime/errors.h"
#include "runtime/string.h"

namespace snowman {

namespace {

VM* createVM() {
  VM* v = new VM();
#define SNOWMAN_CREATE_SYMBOL(name) v->symbols.name = make<Symbol>(Value(atom(u"Symbol." #name)));
  SNOWMAN_WELL_KNOWN_SYMBOLS(SNOWMAN_CREATE_SYMBOL)
#undef SNOWMAN_CREATE_SYMBOL
  return v;
}

// The headroom left below vm().stackLimit for throwing the RangeError and unwinding.
constexpr size_t kStackMargin = 128 * 1024;

// vm().stackLimit for when no one has set it (tools, tests): the main thread's stack bottom plus the
// headroom. Whoever switches stacks sets it the same way (stack low + headroom).
const char* defaultStackLimit() {
  pthread_attr_t attr;
  void* low = nullptr;
  size_t size = 0;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    pthread_attr_getstack(&attr, &low, &size);
    pthread_attr_destroy(&attr);
  }
  if (!low || size <= 2 * kStackMargin) return nullptr;
  return static_cast<const char*>(low) + kStackMargin;
}

}  // namespace

VM& vm() {
  static VM* instance = createVM();
  return *instance;
}

Result<void> VM::runJobs() {
  while (!jobs_.empty()) {
    Job* job = jobs_.front();
    jobs_.pop_front();
    Result<void> result = job->run();
    if (result.isThrow()) return result;
  }
  return {};
}

void VM::traceRoots(Tracer& tracer) {
#define SNOWMAN_TRACE_SYMBOL(name) tracer.visit(reinterpret_cast<Cell*>(symbols.name));
  SNOWMAN_WELL_KNOWN_SYMBOLS(SNOWMAN_TRACE_SYMBOL)
#undef SNOWMAN_TRACE_SYMBOL
  for (ExecutionContext* c = running; c; c = c->previous) {
    tracer.visit(reinterpret_cast<Cell*>(c->realm));
    tracer.visit(reinterpret_cast<Cell*>(c->function));
  }
  tracer.visit(reinterpret_cast<Cell*>(initialRealm));
  for (Realm* realm : realms) tracer.visit(reinterpret_cast<Cell*>(realm));
  for (auto& [key, symbol] : symbolRegistry) {
    tracer.visit(reinterpret_cast<Cell*>(key));
    tracer.visit(reinterpret_cast<Cell*>(symbol));
  }
  for (Job* job : jobs_) tracer.visit(job);
}

Result<void> CheckStack() {
  char here;
  VM& v = vm();
  if (!v.stackLimit) {
    v.stackLimit = defaultStackLimit();
    if (!v.stackLimit) return {};
  }
  if (&here < v.stackLimit) return ThrowRangeError("Maximum call stack size exceeded");
  return {};
}

}  // namespace snowman
