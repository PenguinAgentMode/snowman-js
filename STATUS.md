# Status

| Workstream | Owner (agent) | Branch | Directories | State |
| --- | --- | --- | --- | --- |
| memcap | memcap | `ws/memcap` | `src/interp/coroutine.{h,cpp}`, stack-size users; `t262/runner/run.mjs` (authorized) | watchdog merged (8e39780); engine stack budgets + `ulimit -v` in progress |
| survey | survey | `ws/survey` | `tools/lead/survey-full.md` (analysis only) | running the 10,311 non-ES-core files of the full suite; failure clusters |
| parser, regexp, interp, modules | (closed) | | | merged before M8 |
| runtime, interp2 | (closed; previous team) | `ws/runtime` (+1), `ws/interp2` (+4) | | unmerged perf/IsHTMLDDA commits; to gate on ES-core before merging |

Lead-owned: `main`, `src/main.cpp`, `src/engine.h`, `src/runtime/*.h` (interfaces), `INTERFACES.md`, `PLAN.md`,
`t262/`, `results/`, `STATUS.md`, `Makefile`.

## Latest result

- Engine commit: f294bec1b (main)
- ES-core (`results/f294bec1b.json`): **42,433 / 43,305 files (97.99%)**; scenarios 81,138 / 82,724; test/harness
  116 / 116; es5_language 4,485 / 4,538 (98.8%); vs eff4d1fa9: +3,440 newly passing, 0 regressions
- Parse-only (`results/2551c6978-parse.json`): 43,305 / 43,305
- Milestone: **M8** (every rung of the ladder reached)
- Command: `node t262/runner/milestones.mjs results/f294bec1b.json --parse-only results/2551c6978-parse.json`
- Known ceiling: ~104 module tests import their own file and can't pass under the current runner
  (src/interp/module_test_exclusions.txt; decision with the supervisor).

## Memory (incident 2026-10-13: the lead's machine was OOM-killed)

- Machines: 4 GB enforced (the whole machine dies), gVisor. `nproc`/`free` show the host: build `make -j4`, test
  `--jobs 4`.
- gVisor commits private anonymous memory in 2 MB-aligned units clipped to the mapping: every started 1 MB
  generator/async coroutine stack costs a full 1 MB resident. 300 live generators = 322 MB, 3,000 = 3.1 GB.
- Recursive `yield* g()` takes a fresh coroutine stack per level and never hits CheckStack: ~1 MB per 60 µs,
  unbounded. Never run such probes without `ulimit -v` in the same shell.
- The main program runs on a 1 GB coroutine stack (MAP_NORESERVE): any `ulimit -v` <= 1 GB kills the engine at
  startup. The 35 tail-call-optimization tests (100k-deep calls, ~3–5 KB of C stack per JS call, 220–520 MB
  resident) pass only because of it: there are no proper tail calls yet.
- Guard 1 (main 8e39780): run.mjs watchdog SIGKILLs an engine above 700 MB resident (`MemoryLimit`). In gVisor
  /proc is the outer PID namespace; the watchdog finds the child by PPid + command line.
- Guard 2 (memcap, in progress): main stack ~640 MB, 256 KB coroutine stacks, a live coroutine-stack budget,
  coroutine nesting counted against the stack limit (RangeError), then `ulimit -v` in run.mjs. After proper tail
  calls land, the main stack shrinks to ~64 MB.

## Waivers

Each line is a known regression, with its reason.

- TypedArray/prototype/copyWithin/coerced-values-{start,end}-detached(-prototype).js (3): passed by accident before
  cf35ee1, now near the 10 s timeout; interp2's speed work (waived until it lands).
