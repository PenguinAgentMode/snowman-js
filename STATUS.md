# Status

| Workstream | Owner (agent) | Branch | Directories | State |
| --- | --- | --- | --- | --- |
| memcap | memcap | `ws/memcap` | `src/interp/coroutine.{h,cpp}`, stack-size users; `t262/runner/run.mjs` (authorized) | watchdog merged (8e39780); 85210ca13: main stack 480 MB, 256 KB coroutine stacks, stack budget, parser depth guard, `ulimit -v` 1024 MB; ES-core gate running |
| temporal | temporal | `ws/temporal` | `src/builtins/temporal/` core (temporal.h, parser, options) + Duration, Plain* (3,166 files); additive Intrinsics in realm.h; Date.prototype.toTemporalInstant | started: shared core first |
| temporal2 | temporal2 | `ws/temporal2` | `src/builtins/temporal/{instant,zoned_date_time,now,time_zone}.cpp`, `time_zone.h`, `tz*`, `tools/temporal/` (1,434 files + tz data for intl402/Temporal) | started: time_zone.h first |
| intl | intl | `ws/intl` | `src/builtins/intl/`, `tools/intl/`; additive Intrinsics in realm.h | started: plumbing for all constructors first |
| builtins | builtins | `ws/builtins` | promise/error/iterator/regexp/arraybuffer/typedarray/dataview/atomics/uint8array_base64 built-ins; additive `runtime/arraybuffer.h` | started: allKeyed, Error.stack, Iterator.join, legacy RegExp statics, immutable ArrayBuffer (228 files) |
| survey | (closed) | `ws/survey` | | report merged d44aba3: `tools/lead/survey-full.md` |
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

## Decisions

- 2026-10-13, supervisor: no ICU. Locale data (intl) and tz data (temporal2) are generated from node by checked-in
  scripts. Each .inc records the script, the command and node's versions, and regenerating it is byte-identical.
- 2026-10-13, supervisor: module tests run in place (INTERFACES.md §1, preludes/epilogues), so the 104
  self-importing module tests can pass. Recorded as a runner change, with a per-test before/after on every module
  test in the full suite. A prelude/epilogue error never satisfies a negative test.
- Expected flips: when `import defer` / `import source` land, most of the 242 proposal negative-syntax tests that
  pass today only because the parser rejects all of `import.defer`/`import.source` may change status. That
  branch's gate lists them separately as expected, with each one checked against its specific early error.

## Gate baseline

- Until memcap's gated ES-core run lands: `git show origin/results:results/7e957a05a/es-core-estimated.json >
  /tmp/baseline.json` (43,059 passing files: f294bec1b's passing files plus its failures that pass on 7e957a05a in
  survey's re-run). Check: `node tools/lead/gate.mjs /tmp/baseline.json /tmp/core.jsonl` (AGENTS.md).
- Then: memcap's run at its branch sha becomes `results/<sha>.json` on main, the first measured baseline since
  f294bec1b, followed by the first `tools/lead/measure.sh suite` run on main.

## Survey interim (main 7e957a05a)

- Extra set (full minus ES-core): 10,311 files, 1,709 pass. Temporal 0 / 4,605; intl402 25 / 3,382 (2,029 are
  intl402/Temporal); proposals 243 / 815 (242 of those passes are negative-syntax tests, likely passing by
  accident); staging 1,432 / 1,483.
- ES-core: of f294bec1b's 872 failing files, 626 pass on 7e957a05a. Estimate ≈ 43,059 / 43,305 (99.43%), not
  measured. The remaining 246: 104 self-import (runner), 114 Atomics `$262.agent`/CanBlock (host), 25 IsHTMLDDA
  (host; ws/runtime has the runtime part), 3 engine bugs.

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
