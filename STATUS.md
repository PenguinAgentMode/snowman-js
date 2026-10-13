# Status

| Workstream | Owner (agent) | Branch | Directories | State |
| --- | --- | --- | --- | --- |
| memcap | memcap (reopened: measurement + runner) | `ws/memcap`, `ws/runner`, `ws/measure` | `t262/`, `src/main.cpp`, `src/interp/cli.cpp` for the runner change | runner change gated: module tests 613 → 719 (+106, 0 newly failing); full-suite script check running; then post-merge measurement, then ws/interp2 + ws/runtime. Earlier: merged 5ff85e8: 480 MB main stack, 256 KB coroutine stacks + budgets (RangeError), parser depth guard, `ulimit -v` 1024 MB; 0 regressions. Open: dropped suspended generators keep their stack until exit (cap ~1,000; needs 2 heap.h methods); no CheckStack on nested array/object literals (SIGSEGV past ~400 levels in a generator; no test hits it) |
| temporal | temporal | `ws/temporal` | `src/builtins/temporal/` core (temporal.h, parser, options, calendar) + Duration, Plain*; Intrinsics in realm.h; Date.prototype.toTemporalInstant | merged 00dc9d9 (built-ins/Temporal 3,047 alone; 4,605 / 4,605 with ws/temporal2 in a trial merge). Next: non-ISO calendars for intl402/Temporal |
| temporal2 | temporal2 | `ws/temporal2` | `src/builtins/temporal/{instant,zoned_date_time,now,time_zone}.cpp`, `time_zone.h`, `tz*`, `tools/temporal/` | merge request next (Instant 467/467, ZonedDateTime 899/901, Now 66/66). Next: module proposals (import defer/source/text/bytes; owns `src/parser/`, `src/interp/module.*` for it) + CoverInitializedName |
| intl | intl | `ws/intl` | `src/builtins/intl/`, `tools/intl/`; additive Intrinsics in realm.h | started: plumbing for all constructors first |
| builtins | builtins | `ws/builtins` | promise/error/iterator/regexp/arraybuffer/typedarray/dataview/atomics/uint8array_base64 built-ins; additive `runtime/arraybuffer.h`, `runtime/typedarray.h` | merged 5b81747 (allKeyed 89/89, Error.stack 35/35, Iterator.join 18/18); gating legacy RegExp (26/26) and immutable ArrayBuffer (61/66). Next: ShadowRealm (importValue's loader hook after temporal2), 3 ES-core bugs, staging engine bugs + Annex B |
| survey | survey (IDLE; closes but stays listed: agent-system fault, reported) | `ws/survey` | | report merged d44aba3. Holds a machine: reopen it with agent_send for the next area instead of spawning; first wait for its orphaned node run (2 × 768 MB heap) to finish |
| parser, regexp, interp, modules | (closed) | | | merged before M8 |
| runtime, interp2 | (closed; previous team) | `ws/runtime` (+1), `ws/interp2` (+4) | | unmerged perf/IsHTMLDDA commits; to gate on ES-core before merging |

Lead-owned: `main`, `src/main.cpp`, `src/engine.h`, `src/runtime/*.h` (interfaces), `INTERFACES.md`, `PLAN.md`,
`t262/`, `results/`, `STATUS.md`, `Makefile`.

## Latest result

First measured full-suite run: **main 8a5df0b93** (`tools/lead/measure.sh suite results/85210ca13.json`, 4 jobs,
2,940 s; aggregates `results/8a5df0b93-full.json`, `results/8a5df0b93.json`; JSONL on the results branch as
`results/8a5df0b93/suite.jsonl`).

- milestones.mjs: highest **M8**; **ES-core: 43,059 / 43,305 files (99.43%)**; **full suite: 44,766 / 53,616 files
  (83.49%)**; scenarios 85,571 / 102,994. Gate vs 85210ca13: 0 regressions.
- Command: `node t262/runner/milestones.mjs results/8a5df0b93.json --full results/8a5df0b93-full.json`
- Per cluster (`node tools/lead/clusters.mjs results/8a5df0b93-full.json`):

| Cluster | Passing (8a5df0b93) | Files | % |
| --- | ---: | ---: | ---: |
| ES-core | 43,059 | 43,305 | 99.43 |
| built-ins/Temporal | 0 | 4,605 | 0.00 |
| intl402/Temporal | 0 | 2,029 | 0.00 |
| intl402 (other) | 25 | 1,353 | 1.85 |
| staging | 1,430 | 1,483 | 96.43 |
| proposals (in ES-core dirs) | 252 | 841 | 29.96 |
| **full suite** | **44,766** | **53,616** | **83.49** |

- Memory cap: 5 staging/sm files fail only under it (full suite only, not gated): extensions/recursion.js (932 MB
  peak), RegExp/unicode-{class-,}braced.js (444 MB resident but > 1 GB address space), regress/regress-610026.js
  (2.3 GB), String/replace-math.js (OOM even at 3 GB). Engine memory use, not a cap problem.

## Merges awaiting a post-merge measurement

Not in 8a5df0b93's run; the next `measure.sh suite` on main covers them:
- 5b81747 ws/builtins a87b211 (allKeyed, Error.stack, Iterator.join), gated on 6d73e90.
- 00dc9d9 ws/temporal 7526397 (Temporal core + Duration + Plain*), gated on 8a5df0b.
- 8e85e96 ws/builtins d0423a1 (legacy RegExp statics 26/26 + Function.prototype.toString fix in
  builtins/function_prototype.cpp), gated on cfb908c (cf26874 alone: 1 regression, fixed by d0423a1; Function
  893/893 on the fix).

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

- `results/85210ca13.json` on main (43,059 passing files). Check: `node tools/lead/gate.mjs results/85210ca13.json
  /tmp/core.jsonl` (AGENTS.md). For full-suite branches: `results/<sha>-full.json` once the first suite run lands.

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
