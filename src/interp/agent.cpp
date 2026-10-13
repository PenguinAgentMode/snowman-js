// $262.agent: agent threads, mailboxes, the process-wide waiter lists and the report queue (agent.h).
#include "interp/agent.h"

#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <list>
#include <map>
#include <mutex>

#include "interp/coroutine.h"
#include "interp/interpreter.h"
#include "parser/parser.h"
#include "runtime/errors.h"
#include "runtime/heap.h"
#include "runtime/operations.h"
#include "runtime/string.h"
#include "runtime/vm.h"

namespace snowman::interp {

namespace {

// An agent's JavaScript runs on a coroutine stack of this size, counted in Coroutine::kStackBytesBudget with every
// other started stack of the process; its pthread only resumes that coroutine.
constexpr size_t kAgentStackSize = size_t(16) << 20;
constexpr size_t kAgentThreadStackSize = size_t(1) << 20;

struct BroadcastAck {
  std::mutex m;
  std::condition_variable cv;
  size_t remaining = 0;
  void done() {
    std::lock_guard<std::mutex> lock(m);
    remaining--;
    cv.notify_all();
  }
};

// A thread's mailbox: broadcasts (agents only) and the async waiters other agents notified.
struct Mailbox {
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::pair<BroadcastMessage, std::shared_ptr<BroadcastAck>>> broadcasts;
  std::vector<uint64_t> wakeups;
  bool finished = false;
};

std::mutex gAgentsMutex;
std::vector<Mailbox*> gAgents;  // the agents' mailboxes (never freed)
std::atomic<bool> gStarted{false};
std::atomic<int> gRunning{0};

thread_local Mailbox* tMailbox = nullptr;
Mailbox* myMailbox() {
  if (!tMailbox) tMailbox = new Mailbox();
  return tMailbox;
}

// The waiter lists: one FIFO list per (block, byte index), for every agent; a sync waiter is a condition variable
// of a blocked thread, an async one an id in its thread's own table (atomics_wait_async.cpp).
struct WaitEntry {
  std::condition_variable* cv;
  bool* notified;
  Mailbox* owner;
  uint64_t asyncId;
};
using WaitKey = std::pair<const SharedBlock*, size_t>;
std::mutex gWaitMutex;
std::map<WaitKey, std::list<WaitEntry>> gWaitLists;
std::atomic<uint64_t> gNextAsyncId{1};

std::mutex gReportMutex;
std::deque<std::u16string> gReports;

// The agent thread's script state: the receiveBroadcast callback (a root) and leaving().
class AgentLocal final : public RootProvider {
 public:
  void traceRoots(Tracer& tracer) override { tracer.visit(callback); }
  Value callback = Value::undefined();
  bool leaving = false;
};
thread_local AgentLocal* tAgentLocal = nullptr;

struct AgentStart {
  std::u16string source;
  Mailbox* mailbox;
};

std::string describe(Value thrown) {
  if (thrown.isObject()) {
    Result<Value> name = Get(thrown.asObject(), PropertyKey(u"name"));
    Result<Value> message = Get(thrown.asObject(), PropertyKey(u"message"));
    if (name.isNormal() && message.isNormal() && name.value().isString() && message.value().isString())
      return name.value().asString()->toUtf8() + ": " + message.value().asString()->toUtf8();
  }
  Result<String*> text = ToString(thrown);
  return "Uncaught: " + (text.isNormal() ? text.value()->toUtf8() : std::string("?"));
}

// An uncaught error in an agent fails the whole test at once: print it as the main agent would, end the process.
[[noreturn]] void agentDies(Value thrown) {
  std::string line = describe(thrown) + "\n";
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fflush(nullptr);
  std::_Exit(1);
}

void runJobsOrDie() {
  Result<void> jobs = vm().runJobs();
  if (jobs.isThrow()) agentDies(jobs.thrown());
}

void agentMain(Coroutine*, void* data) {
  auto* start = static_cast<AgentStart*>(data);
  parser::stackLimit = &vm().stackLimit;
  vm().canBlock = true;
  tAgentLocal = new AgentLocal();
  Realm* realm = CreateRealm();
  vm().initialRealm = realm;
  setUpRealm(realm);
  heap().setCollectorEnabled(true);
  parser::ParseResult parsed = parser::parseProgram(std::move(start->source));
  if (!parsed.ok()) {
    std::string line = "SyntaxError: " + parsed.error->message + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
    std::_Exit(1);
  }
  Result<Value> result = ScriptEvaluation(snowman::make<ScriptRecord>(realm, parsed.program));
  if (result.isThrow()) agentDies(result.thrown());
  runJobsOrDie();
  // The event loop: broadcasts until leaving() (a later broadcast skips a finished agent).
  Mailbox* mb = start->mailbox;
  while (true) {
    std::optional<std::pair<BroadcastMessage, std::shared_ptr<BroadcastAck>>> next;
    {
      std::unique_lock<std::mutex> lock(mb->m);
      if (mb->broadcasts.empty() && tAgentLocal->leaving) return;
      mb->cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return !mb->broadcasts.empty(); });
      if (mb->broadcasts.empty()) continue;
      next = std::move(mb->broadcasts.front());
      mb->broadcasts.pop_front();
    }
    next->second->done();
    Value callback = tAgentLocal->callback;
    if (!IsCallable(callback)) continue;
    auto* sab = snowman::make<ArrayBufferObject>(realm->intrinsic(Intrinsic::SharedArrayBufferPrototype), next->first.block,
                                        next->first.maxByteLength);
    Result<Value> called = Call(callback, Value::undefined(), {Value(sab), Value::number(next->first.id)});
    if (called.isThrow()) agentDies(called.thrown());
    runJobsOrDie();
  }
}

void finishAgent(Mailbox* mb) {
  std::lock_guard<std::mutex> lock(mb->m);
  mb->finished = true;
  for (auto& [message, ack] : mb->broadcasts) ack->done();
  mb->broadcasts.clear();
}

void* agentThread(void* data) {
  auto* start = static_cast<AgentStart*>(data);
  tMailbox = start->mailbox;
  {
    Coroutine coroutine(agentMain, start, kAgentStackSize);
    if (!coroutine.resume()) {
      std::fputs("fatal: cannot map an agent stack\n", stdout);
      std::fflush(stdout);
      std::_Exit(70);
    }
  }
  finishAgent(start->mailbox);
  delete start;
  gRunning--;
  return nullptr;
}

std::chrono::steady_clock::time_point deadlineAfter(double ms) {
  return std::chrono::steady_clock::now() +
         std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(ms));
}

}  // namespace

double MonotonicMs() {
  static const auto origin = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - origin).count();
}

bool StartAgent(std::u16string source) {
  auto* start = new AgentStart{std::move(source), new Mailbox()};
  {
    std::lock_guard<std::mutex> lock(gAgentsMutex);
    gAgents.push_back(start->mailbox);
  }
  myMailbox();
  gStarted = true;
  gRunning++;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kAgentThreadStackSize);
  pthread_t thread;
  int error = pthread_create(&thread, &attr, agentThread, start);
  pthread_attr_destroy(&attr);
  if (error) {
    gRunning--;
    finishAgent(start->mailbox);
    delete start;
    return false;
  }
  return true;
}

bool AgentsStarted() { return gStarted; }
bool AgentThreadsRunning() { return gRunning > 0; }

void SetBroadcastCallback(Value callback) {
  if (tAgentLocal) tAgentLocal->callback = callback;
}
void SetLeaving() {
  if (tAgentLocal) tAgentLocal->leaving = true;
}
bool InAgentThread() { return tAgentLocal != nullptr; }

void BroadcastToAgents(const BroadcastMessage& message) {
  auto ack = std::make_shared<BroadcastAck>();
  std::vector<Mailbox*> targets;
  {
    std::lock_guard<std::mutex> lock(gAgentsMutex);
    targets = gAgents;
  }
  {
    std::lock_guard<std::mutex> lock(ack->m);
    for (Mailbox* mb : targets) {
      std::lock_guard<std::mutex> mbLock(mb->m);
      if (mb->finished) continue;
      ack->remaining++;
      mb->broadcasts.emplace_back(message, ack);
      mb->cv.notify_all();
    }
  }
  std::unique_lock<std::mutex> lock(ack->m);
  ack->cv.wait(lock, [&] { return ack->remaining == 0; });
}

void PushReport(std::u16string report) {
  std::lock_guard<std::mutex> lock(gReportMutex);
  gReports.push_back(std::move(report));
}

std::optional<std::u16string> PopReport() {
  std::lock_guard<std::mutex> lock(gReportMutex);
  if (gReports.empty()) return std::nullopt;
  std::u16string report = std::move(gReports.front());
  gReports.pop_front();
  return report;
}

const char16_t* WaitSync(const SharedBlock* block, size_t byteIndex, const std::function<bool()>& stillEqual,
                         double timeoutMs) {
  std::unique_lock<std::mutex> lock(gWaitMutex);
  if (!stillEqual()) return u"not-equal";
  std::condition_variable cv;
  bool notified = false;
  WaitKey key{block, byteIndex};
  std::list<WaitEntry>& list = gWaitLists[key];
  auto entry = list.insert(list.end(), WaitEntry{&cv, &notified, nullptr, 0});
  if (!std::isfinite(timeoutMs) || timeoutMs > 1e12) {
    cv.wait(lock, [&] { return notified; });
  } else {
    cv.wait_until(lock, deadlineAfter(timeoutMs), [&] { return notified; });
  }
  if (notified) return u"ok";
  list.erase(entry);
  if (list.empty()) gWaitLists.erase(key);
  return u"timed-out";
}

uint64_t AddAsyncWaiter(const SharedBlock* block, size_t byteIndex, const std::function<bool()>& stillEqual) {
  Mailbox* owner = myMailbox();
  std::lock_guard<std::mutex> lock(gWaitMutex);
  if (!stillEqual()) return 0;
  uint64_t id = gNextAsyncId++;
  gWaitLists[{block, byteIndex}].push_back(WaitEntry{nullptr, nullptr, owner, id});
  return id;
}

bool RemoveAsyncWaiter(const SharedBlock* block, size_t byteIndex, uint64_t id) {
  std::lock_guard<std::mutex> lock(gWaitMutex);
  auto it = gWaitLists.find({block, byteIndex});
  if (it == gWaitLists.end()) return false;
  for (auto e = it->second.begin(); e != it->second.end(); ++e) {
    if (e->asyncId == id) {
      it->second.erase(e);
      if (it->second.empty()) gWaitLists.erase(it);
      return true;
    }
  }
  return false;
}

size_t NotifyWaiters(const SharedBlock* block, size_t byteIndex, double count, std::vector<uint64_t>& ownAsync) {
  Mailbox* self = myMailbox();
  std::lock_guard<std::mutex> lock(gWaitMutex);
  auto it = gWaitLists.find({block, byteIndex});
  if (it == gWaitLists.end()) return 0;
  size_t woken = 0;
  while (!it->second.empty() && double(woken) < count) {
    WaitEntry e = it->second.front();
    it->second.pop_front();
    woken++;
    if (e.cv) {
      *e.notified = true;
      e.cv->notify_one();
    } else if (e.owner == self) {
      ownAsync.push_back(e.asyncId);
    } else {
      std::lock_guard<std::mutex> mbLock(e.owner->m);
      e.owner->wakeups.push_back(e.asyncId);
      e.owner->cv.notify_all();
    }
  }
  if (it->second.empty()) gWaitLists.erase(it);
  return woken;
}

std::vector<uint64_t> TakeAsyncWakeups() {
  Mailbox* mb = myMailbox();
  std::lock_guard<std::mutex> lock(mb->m);
  std::vector<uint64_t> out;
  out.swap(mb->wakeups);
  return out;
}

void WaitForMailbox(double untilMs) {
  Mailbox* mb = myMailbox();
  std::unique_lock<std::mutex> lock(mb->m);
  double wait = untilMs - MonotonicMs();
  if (wait <= 0) return;
  // At most 100 ms at a time: the caller re-checks its timeouts and loops.
  mb->cv.wait_until(lock, deadlineAfter(std::min(wait, 100.0)),
                    [&] { return !mb->wakeups.empty() || !mb->broadcasts.empty(); });
}

}  // namespace snowman::interp
