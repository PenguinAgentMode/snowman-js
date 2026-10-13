// $262.agent (test262's INTERPRETING.md): agents are threads, each with its own VM, heap and realm; they share only
// SharedArrayBuffer data blocks. This file holds what crosses threads: the agents' mailboxes, the process-wide
// waiter lists of Atomics.wait / waitAsync / notify, and the report queue (notes/agent-design.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "runtime/arraybuffer.h"

namespace snowman::interp {

// A SharedArrayBuffer sent to the agents by $262.agent.broadcast.
struct BroadcastMessage {
  std::shared_ptr<SharedBlock> block;
  std::optional<size_t> maxByteLength;
  double id;
};

// Starts an agent running `source` (a Script); false if no thread or stack could be had.
bool StartAgent(std::u16string source);
// True once an agent was started: the process then ends with _exit (agent threads never hold it up).
bool AgentsStarted();
// The agent side of $262.agent: receiveBroadcast(callback), leaving(), and whether this thread is an agent.
void SetBroadcastCallback(Value callback);
void SetLeaving();
bool InAgentThread();
// broadcast(sab, id): posts to every running agent and returns once each one has taken the message.
void BroadcastToAgents(const BroadcastMessage& message);
// report(value) / getReport(): the process-wide report queue.
void PushReport(std::u16string report);
std::optional<std::u16string> PopReport();

// Atomics.wait on a shared block (byteIndex within it): under the waiter-list lock `stillEqual()` re-reads the value;
// then the thread blocks until notified or timeoutMs (+inf: forever). Returns u"ok", u"timed-out" or u"not-equal".
const char16_t* WaitSync(const SharedBlock* block, size_t byteIndex, const std::function<bool()>& stillEqual,
                         double timeoutMs);
// Atomics.waitAsync: adds an async waiter of the running thread; 0 if `stillEqual()` fails under the lock.
uint64_t AddAsyncWaiter(const SharedBlock* block, size_t byteIndex, const std::function<bool()>& stillEqual);
// An async waiter's timeout: removes it; false if a notify took it first (its wakeup is in this thread's mailbox).
bool RemoveAsyncWaiter(const SharedBlock* block, size_t byteIndex, uint64_t id);
// Atomics.notify: removes up to `count` waiters in FIFO order and wakes them. Async waiters of the running thread are
// appended to `ownAsync` (the caller resolves them); other threads' are posted to their mailboxes.
size_t NotifyWaiters(const SharedBlock* block, size_t byteIndex, double count, std::vector<uint64_t>& ownAsync);
// The async waiters of the running thread that other agents notified since the last call.
std::vector<uint64_t> TakeAsyncWakeups();
// Blocks the running thread until something arrives in its mailbox, or until untilMs (monotonic ms).
void WaitForMailbox(double untilMs);
// True while some agent thread is running (an infinite async wait may still be notified).
bool AgentThreadsRunning();
double MonotonicMs();

}  // namespace snowman::interp
