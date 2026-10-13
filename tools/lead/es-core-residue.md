# ES-core residue (measured on main c21bcd82e)

ES-core 43,163 / 43,305 (99.67%): 142 failing files. `node tools/lead/nodegap.mjs results/c21bcd82e.json`:
node passes 4 of them, fails the other 138 too.

## By cause

| Files | Cause | Owner / fix |
| ---: | --- | --- |
| 112 | `$262.agent`: Atomics/wait 43, Atomics/waitAsync 53, Atomics/notify 16 | host + runtime: agents (below) |
| 25 | `$262.IsHTMLDDA` (annexB) | ws/runtime-r 40af25e: runtime part (351e794) + host hook / typeof |
| 2 | Atomics/wait/{,bigint/}cannot-suspend-throws.js: `flags: [CanBlockIsFalse]` | runner: run with a non-blocking main agent |
| 2 | Promise/try/avoids-wrap{,-for-subclass}.js | fixed on main after c21bcd8 (builtins; Promise.try through PromiseResolve) |
| 1 | Object/freeze/typedarray-backed-by-resizable-buffer.js | ws/runtime-r (TypedArray [[PreventExtensions]]) |

The 4 node passes and we don't: Atomics/waitAsync/{,bigint/}true-for-timeout.js and
returns-result-object-value-is-promise-resolves-to-timed-out.js (they use `$262.agent.sleep` /
`monotonicNow` only, no second agent), and the Object/freeze file above.

## `$262.agent` design estimate

API (INTERPRETING.md): `start(src)`, `broadcast(sab[, id])`, `getReport()`, `sleep(ms)`, `monotonicNow()`;
inside an agent `receiveBroadcast(cb)`, `report(v)`, `leaving()`.

1. Cheap first step (+3 files): `$262.agent.sleep` and `monotonicNow` on the main agent, no threads.
2. Per-thread engine state, made `thread_local` (one VM per agent; cells never cross threads): `vm()`,
   `heap()`, the atom table and its roots (runtime/string.cpp), `emptyString()`, coroutine globals (gCurrent,
   gStackPool, gStackBytes), parser `gChunk`, `gScriptDirectory`, the waitAsync `WaiterLists`, plus
   `parser::stackLimit`. Read-only caches (unicode property lookups, the realm registration list, RegExpCheck)
   are initialised before the first agent starts and shared.
3. SharedArrayBuffer: the data block becomes a refcounted, fixed-address block (`shared_ptr`, max size reserved
   for growable SABs) shared by one SharedArrayBufferObject per agent; broadcast hands the block, not the cell.
4. Atomics: real atomic accesses on shared blocks (`std::atomic_ref`); one process-wide waiter list per
   (block, index) under a mutex, with a condition variable per waiter; `wait` blocks with `wait_until`,
   `notify` wakes FIFO; async waiters of another agent are woken through that agent's (now thread-safe) job
   queue mailbox.
5. Host (src/interp/host.cpp): `start` runs the source on a `std::thread` with its own VM, realm and a small
   main stack (16-32 MB: 4 agents x 480 MB would not fit ulimit -v 1 GB); `broadcast` posts to every agent and
   waits until each has taken it; `report`/`getReport` share a mutex-protected queue; `sleep` sleeps.
6. CanBlock: our main agent can block. The 2 `CanBlockIsFalse` tests need the runner to pass a flag
   (e.g. `--can-block=false`) for that flag; the 7 `CanBlockIsTrue` tests keep the default.

Size: about 800-1,200 lines, one workstream, touching runtime (vm/heap/string singletons, arraybuffer,
atomics) and interp (host, coroutine globals). Risks: thread-safety of the singletons (a missed global is a
data race, not a crash, until it is), and timing flakiness: the agent tests sleep and use timeouts, and run 4
jobs at once. Gain: up to 112 files (+2 with the runner flag), which node's host does not pass either.

## The memory-hungry staging tests (full suite, not ES-core)

Measured with scaled-down inputs under `ulimit -v 1 GB` (per-unit costs; base engine 16.6 MB).

| Test | Peak | Root cause | Fix |
| --- | --- | --- | --- |
| sm/extensions/recursion.js | 929 MB (MemoryLimit) | infinite plain recursion leaves 480 MB of main stack resident after unwinding (656 MB alone with its frames' heap), then a generator recursion maps one 256 KB stack per level (1,024 = 256 MB) | ws/stacktrim 6b67bbf: coroutine budget 128 MB + madvise(DONTNEED) of the dead main stack before mapping a new stack: 657 MB, passes |
| sm/regress/regress-610026.js | 2.3 GB (out of memory) | ~400 B per empty `{}` block, almost all in the parser (parse-only 398 B/block, evaluation +10): every block opens a parser `Scope` (280 B: 4 hash containers + 2 vectors) that `scopePool_` keeps until the parse ends, plus the 88 B BlockStatement; the test evals ~2M then ~4M blocks | parser: free or reuse a Scope at exitScope (live scopes are only the nesting depth): ~100 B/block |
| sm/RegExp/unicode-braced.js, unicode-class-braced.js | 444 MB resident, > 1 GB address space | a 16M-character pattern (`"0".repeat(2**24)`) through `eval` of a regex literal costs ~20 B per character (`new RegExp` ~10 B; the string itself 2 B), i.e. ~10 full copies. Inferred from the per-character cost, not traced line by line; the likely copies are the eval source, the lexer token, the AST, the validator's and regexp parser's code-point arrays (4 B/char) and the RegExp source; freed 32-64 MB blocks stay mapped (M_TRIM_THRESHOLD 1 GB), so the address space passes the 1 GB cap | fewer copies on the eval/RegExp path (pass views; validate and compile from UTF-16 without a code-point copy) |
| sm/String/replace-math.js | 1.3 GB (out of memory at 3 GB too) | a replacement over the 2^30 string length limit was built before the check | fixed on main by builtins (GetSubstitution checks the length first: RangeError) |
