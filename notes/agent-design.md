# `$262.agent`: design (builtins)

Goal: the 109 ES-core files under test/built-ins/Atomics that call `$262.agent.start` (wait, waitAsync, notify), on top of
step 1 (merged separately: `sleep`, `monotonicNow`, `getReport` on the main agent, +3 files).

## Hard constraints (from the supervisor), designed to

1. **Address space.** The runner runs the engine under `ulimit -v 1024 MB`, which already holds the 480 MB main-stack
   reservation. Agent threads get small stacks: **16 MB each** (test262 agent bodies are shallow), counted in the same
   started-stack budget as coroutines (`kStackBytesBudget`, made process-wide). Also: every thread gets its own glibc malloc
   arena (a 64 MB address-space reservation each), so the CLI sets `mallopt(M_ARENA_MAX, 1)` before the first agent starts.
2. **Memory.** Every agent has its own heap, and all heaps count against **one process-wide budget**: the 700 MB RSS
   watchdog sees the whole process.
3. **Testing.** The most agents any Atomics test starts is **4** (`NUMAGENT = 4` in notify/count-defaults-to-infinity-*;
   3 + 1 in notify/{,bigint/}notify-all-on-loc). The test plan runs all 109 agent files, and a 4-agent stress file, under the
   real runner (ulimit -v 1024 MB + the 700 MB watchdog, 4 jobs), not just functionally.
4. **Lifetime.** Agent threads are joined or killed on a normal exit, on an uncaught error and on timeout. A hung
   `Atomics.wait` in an agent can't keep the process alive: there are no child processes (agents are threads), the runner's
   SIGKILL takes all of them, and a normal exit doesn't wait on a blocked agent (shutdown below).

Measured: an empty program peaks at 14 MB RSS, with the harness + atomicsHelper.js 18 MB, each extra realm ~1.2 MB. An agent
(VM + heap + realm + its source) is a few MB plus its stack's touched pages.

## 1. Per-thread engine state (one VM per agent; cells never cross threads)

Each agent thread runs its own VM, heap, atom table and realm. Everything below becomes `thread_local` (static
executable, so TLS is local-exec: one `%fs:` offset, no measurable cost on the hot `vm()`/`heap()` paths). Single-threaded
behaviour is unchanged; this step is its own gated merge.

| State | File (owner) | Change |
| --- | --- | --- |
| `vm()` instance | runtime/vm.cpp (lead interface vm.h unchanged) | `thread_local VM* instance` |
| `heap()` instance | runtime/heap.cpp (heap.h: one additive member, below) | `thread_local` |
| atom table + `AtomRoots` | runtime/string.cpp | `thread_local` |
| `emptyString()` | runtime/string.cpp | `thread_local` |
| index-string cache `small[1024]` | runtime/property.cpp | `thread_local` |
| cached keys `static const PropertyKey` (lastIndex, length) | builtins/regexp.cpp, runtime/array.cpp | `thread_local` (they hold atoms) |
| `gCurrent`, `gStackPool` | interp/coroutine.cpp | `thread_local` |
| `gStackBytes` | interp/coroutine.cpp | `std::atomic<size_t>`, process-wide (constraint 1) |
| `gModuleAsyncEvaluationCount` | interp/module.cpp (temporal2) | `thread_local` |
| `gChunk` (AST arena) | parser/ast.cpp (parser owner) | `thread_local` |
| `parser::stackLimit` (points at the main VM's `stackLimit`) | parser/parser.{h,cpp} | `thread_local`, set per agent |
| `randomState` (Math.random) | builtins/math.cpp | `thread_local` (independent per agent) |
| waitAsync `WaiterLists` | builtins/atomics_wait_async.cpp | process-wide under a mutex (section 3) |
| Intl caches (locale_data.cpp, number_core.cpp), Temporal `time_zone.cpp` cache | intl/survey, temporal2 | `thread_local` |

Shared, read-only once the first agent starts (no change): the builtin registration list (realm.cpp), unicode tables and
`lookupLoneProperty` statics (thread-safe static init), `gScriptDirectory` (set by the CLI before any agent), the
`regExpCreate` / `setRegExpCheck` hooks (same value from every realm; written before agents start), regexp Programs
(immutable, already shared by `shared_ptr<const Program>`).

The symbol registry, well-known symbols and realms list live in the VM, so they are per agent. That's correct: agents
share no objects, only SharedArrayBuffer data blocks.

## 2. Shared data blocks

- `SharedBlock { std::atomic<size_t> byteLength; size_t maxByteLength; uint8_t* data; }`: mmap'd once at `maxByteLength`
  (a growable SAB reserves its max up front, so the address never moves; `grow` only raises `byteLength`). Refcounted
  (`std::shared_ptr<SharedBlock>`), unmapped by the last owner.
- `ArrayBufferObject` for a shared buffer holds `std::shared_ptr<SharedBlock>` instead of `std::vector<uint8_t>`; `data()` and
  `byteLength()` read through it. That's a change to runtime/arraybuffer.h (lead interface): additive (a shared-block member
  + accessor), `data()`/`byteLength()` keep their signatures.
- `broadcast` hands the `shared_ptr`, never the cell: each receiving agent makes its own `SharedArrayBufferObject` in its own
  heap over the same block (one per agent per broadcast).

## 3. Atomics on shared blocks

- Atomic operations (load, store, add, and, compareExchange, exchange, or, sub, xor) on a shared block use
  `std::atomic_ref<T>` (T = the element's 1/2/4/8-byte unsigned type; elements are naturally aligned in a TypedArray).
  Plain TypedArray/DataView element accesses on shared blocks use relaxed `atomic_ref` loads/stores for aligned
  elements and byte-wise relaxed accesses otherwise (no data-race UB, and no tearing within an aligned element).
- **Waiter lists, process-wide:** `std::mutex gWaitMutex`; `std::map<std::pair<SharedBlock*, size_t>, std::list<Waiter*>>`
  (FIFO per location). A `Waiter` has `std::condition_variable cv`, `bool notified`, the owning agent, and for waitAsync a
  mailbox handle + capability id.
  - `Atomics.wait`: lock; atomic-load the value; if not equal return "not-equal"; append the waiter;
    `cv.wait_until(lock, deadline)` until notified, timed out, or shutdown; remove; return "ok" / "timed-out".
  - `Atomics.notify`: lock; take up to `count` waiters from the front; set `notified`; sync waiters get `cv.notify_one()`;
    async waiters get a "resolve ok" message posted to their agent's mailbox. Returns the count.
  - `Atomics.waitAsync`: the same list; a timeout is a deadline the owning agent's event loop watches.
- Non-shared buffers keep today's single-agent code paths.
- AgentCanSuspend() is `vm().canBlock` (memcap adds the flag, the `--can-block=false` CLI/runner flag and the spec's
  `if (!AgentCanSuspend()) throw TypeError` step in Atomics.wait). Agent threads always run with canBlock = true; the main
  agent's comes from the CLI flag.

## 4. Agents (src/interp/host.cpp, src/interp/cli.cpp)

- `$262.agent.start(src)`: `pthread_create` with a 16 MB stack (`pthread_attr_setstacksize`), reserved against the
  process-wide stack budget (a RangeError when the budget would be exceeded). The thread builds a VM, heap and realm
  (`CreateRealm` + `setUpRealm`), installs the agent's `$262.agent` (`receiveBroadcast`, `report`, `leaving`, `sleep`,
  `monotonicNow`), runs `src` as a Script, then runs its event loop: drain jobs; wait on its mailbox (broadcasts, async
  wakeups, waitAsync deadlines) until `leaving()` was called and no work is pending, or shutdown.
- Mailbox: per agent, a mutex + condition variable + queue of plain C++ messages (no cells).
- `broadcast(sab, id)`: post `{shared_ptr<SharedBlock>, id}` to every agent, then block until every agent has taken it and
  called its `receiveBroadcast` callback (INTERPRETING.md).
- `report(v)`: `ToString(v)` as UTF-16, appended to the process-wide report queue (mutex). `getReport()`: pop, as a new
  String in the caller's heap, or null.
- `sleep(ms)` and `monotonicNow()` as in step 1 (process-wide steady clock origin, so all agents agree).

## 5. Memory budget

- `heap.h` (additive): the heap adds its allocated bytes to a process-wide `std::atomic<size_t> gProcessHeapBytes`
  (decremented when it frees). A heap collects when its own budget is reached **or** when the process total passes a soft
  limit (320 MB), so 5 heaps can't each grow to 2x their live size in parallel. The out-of-memory backstop stays as it is
  (ulimit -v + set_new_handler: "fatal: out of memory", exit 71).
- Stacks: main 480 MB + 4 agents x 16 MB = 544 MB of reserved stack inside ulimit -v 1024 MB; the agents' 64 MB come out of
  the coroutine headroom in `kStackBytesBudget` (now one atomic counter for the process).
- Note for memcap (separate): `kStackBytesBudget` (736 MB) is above the 700 MB watchdog (staging sm/extensions/recursion).

## 6. Lifetime and shutdown

- An agent registry (mutex) keeps every thread; nothing is ever detached.
- **Normal exit** (main script + job queue done): `shutdownAgents()` sets `gTerminating`, wakes every waiter
  (`notify_all` on every waiter's cv) and every mailbox. A woken agent sees `gTerminating` at its wait points
  (`Atomics.wait` returns a termination completion that no catch clause sees, like the generator return marker), so its
  script and event loop unwind. Join each agent with a 200 ms deadline; then `fflush(stdout)` and `_exit(code)`, which ends
  any agent still running JS (a busy loop). Exit status is the main agent's.
- **Uncaught error in an agent:** print it as the main agent does (`Name: message`), flush, `_exit(1)`. The test fails
  fast instead of timing out.
- **Uncaught error in the main agent:** print, `shutdownAgents()`, `_exit(1)`.
- **Runner timeout:** SIGKILL of the engine process ends all agent threads; there are no child processes.

## 7. Steps, each its own gated merge

1. Done: `sleep` / `monotonicNow` / `getReport` on the main agent (+3).
2. thread_local state (section 1), no behaviour change; ES-core gate + a timing check (no slowdown on a 2 s CPU benchmark).
3. Shared blocks + real atomics + process-wide waiter lists (sections 2, 3), still one agent; ES-core gate.
4. Agents: start/broadcast/report/getReport/receiveBroadcast/leaving, mailboxes, shutdown, memory budget (sections 4-6);
   the 109 files under the real runner, plus a 4-agent stress file repeated 20x at 4 jobs (timing flakiness), then the gate.
5. memcap's: `--can-block=false` + vm().canBlock for the 2 `CanBlockIsFalse` files.

## Files and owners (approval needed per step)

- Step 2: runtime/{vm,heap,string,property,array}.cpp, builtins/{regexp,math}.cpp, interp/coroutine.cpp, interp/module.cpp
  (temporal2), parser/ast.cpp + parser/parser.{h,cpp} (`stackLimit`), builtins/intl/{locale_data,number_core}.cpp,
  builtins/temporal/time_zone.cpp.
- Step 3: runtime/arraybuffer.{h,cpp} (additive), runtime/typedarray.cpp, builtins/{atomics,atomics_wait_async,
  sharedarraybuffer,dataview}.cpp.
- Step 4: interp/host.cpp, interp/cli.cpp, runtime/heap.{h,cpp} (additive counter), interp/coroutine.{h,cpp} (agent stack
  size, process-wide budget).

## Risks

- A missed global is a silent data race. Mitigation: the inventory greps above (g-prefixed, namespace-scope and
  function-local statics), and a ThreadSanitizer build (`-fsanitize=thread`, outside the runner) over the agent tests before
  step 4's gate.
- Timing: agent tests sleep and use timeouts while 4 jobs run at once. Mitigation: the stress loop in step 4. Known
  waitAsync timing margins of 10-100 ms may still flake under load; any that do get reported, not retried silently.
- Conservative stack scanning stays per thread: each heap scans only its own thread's main and coroutine stacks, which is
  correct because no cell is reachable from another thread.
