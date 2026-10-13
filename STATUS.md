# Status

| Workstream | Owner (agent) | Branch | Directories | State |
| --- | --- | --- | --- | --- |
| memcap | memcap (reopened: measurement + runner) | `ws/memcap`, `ws/runner`, `ws/measure` | `t262/`, `src/main.cpp`, `src/interp/cli.cpp` for the runner change | runner change gated: module tests 613 → 719 (+106, 0 newly failing); full-suite script check running; then post-merge measurement, then ws/interp2 + ws/runtime. Earlier: merged 5ff85e8: 480 MB main stack, 256 KB coroutine stacks + budgets (RangeError), parser depth guard, `ulimit -v` 1024 MB; 0 regressions. Open: dropped suspended generators keep their stack until exit (cap ~1,000; needs 2 heap.h methods); no CheckStack on nested array/object literals (SIGSEGV past ~400 levels in a generator; no test hits it) |
| temporal | temporal | `ws/temporal` | `src/builtins/temporal/` core (temporal.h, parser, options, calendar) + Duration, Plain*; Intrinsics in realm.h; Date.prototype.toTemporalInstant | merged 00dc9d9 (built-ins/Temporal 3,047 alone; 4,605 / 4,605 with ws/temporal2 in a trial merge). Next: non-ISO calendars for intl402/Temporal |
| temporal2 | temporal2 | `ws/temporal2`, `ws/modules2` | Temporal tz layer (merged 9f1e7e5); now `src/parser/`, `src/interp/module.*`, `builtins/abstract_module_source.cpp`, realm.h AbstractModuleSource, `$262.AbstractModuleSource` | module proposals: import defer/source/text/bytes (516 files incl. 242 negative), ShadowRealm loader hook, CoverInitializedName; later parser B.3.2 hoisting + AST size |
| intl | survey (took over: intl's agent died of a context overflow at 1M tokens) | `ws/intl`, `ws/intl-dev` | `src/builtins/intl/`, `tools/intl/`; Intl intrinsics in realm.h; toLocale* in number/bigint/string/array/typedarray/date.cpp | merged 9a1a42b (intl402 non-Temporal 1,069 / 1,353). DateTimeFormat batch ws/intl-dev ccdf619 (1,317 / 1,353; DTF 210/245) to rebase and gate; it turns the 3 Intl-gated staging waivers green |
| builtins | builtins | `ws/builtins` | promise/error/iterator/regexp/arraybuffer/typedarray/dataview/atomics/uint8array_base64 built-ins; additive `runtime/arraybuffer.h`, `runtime/typedarray.h` | merged 5b81747 (allKeyed 89/89, Error.stack 35/35, Iterator.join 18/18); gating legacy RegExp (26/26) and immutable ArrayBuffer (61/66). Next: ShadowRealm (importValue's loader hook after temporal2), 3 ES-core bugs, staging engine bugs + Annex B |
| survey | (reopened for intl, above) | `ws/survey` | | report merged d44aba3 |
| parser, regexp, interp, modules | (closed) | | | merged before M8 |
| runtime, interp2 | (closed; previous team) | `ws/runtime` (+1), `ws/interp2` (+4) | | unmerged perf/IsHTMLDDA commits; to gate on ES-core before merging |

Lead-owned: `main`, `src/main.cpp`, `src/engine.h`, `src/runtime/*.h` (interfaces), `INTERFACES.md`, `PLAN.md`,
`t262/`, `results/`, `STATUS.md`, `Makefile`.

## Latest result

Post-merge run: **main c21bcd82e** (`tools/lead/measure.sh suite results/8a5df0b93.json`, 4 jobs, 2,295 s;
`results/c21bcd82e.json`, `results/c21bcd82e-full.json`; JSONL `results/c21bcd82e/suite.jsonl` on the results branch).

- milestones.mjs: highest **M8**; **ES-core: 43,163 / 43,305 files (99.67%)**; **full suite: 50,887 / 53,616 files
  (94.91%)**; scenarios 97,709 / 102,994.
- ES-core vs 8a5df0b93: +104 newly passing (the runner change), 0 regressions.
- Full suite vs 8a5df0b93: +6,121 files. The gate lists 6 staging files, all waived below: 3 that passed only
  before Intl existed, and 3 flaky DST timeouts.
- Command: `node t262/runner/milestones.mjs results/c21bcd82e.json --full results/c21bcd82e-full.json`

| Cluster | Passing (c21bcd82e) | Files | % | vs 8a5df0b93 |
| --- | ---: | ---: | ---: | ---: |
| ES-core | 43,163 | 43,305 | 99.67 | +104 |
| built-ins/Temporal | 4,605 | 4,605 | 100.00 | +4,605 |
| intl402/Temporal | 197 | 2,029 | 9.71 | +197 |
| intl402 (other) | 1,069 | 1,353 | 79.01 | +1,044 |
| staging | 1,429 | 1,483 | 96.36 | -1 |
| proposals (in ES-core dirs) | 424 | 841 | 50.42 | +172 |
| **full suite** | **50,887** | **53,616** | **94.91** | **+6,121** |

Previous: main 8a5df0b93, full 44,766 / 53,616 (83.49%), ES-core 43,059 (99.43%).

ES-core residue at c21bcd82e (142 files; memcap, `tools/lead/es-core-residue.md` on ws/measure 4f999b1; node
fails 138 of them too):

| Files | Cause | Owner / state |
| ---: | --- | --- |
| 112 | `$262.agent` (Atomics wait 43, waitAsync 53, notify 16) | builtins (design note first): per-thread VM, shared SAB blocks, ~800–1,200 lines; first step `agent.sleep`/`monotonicNow` (+3) |
| 25 | `$262.IsHTMLDDA` (annexB) | memcap ws/runtime-r 40af25e, gating |
| 2 | `flags: [CanBlockIsFalse]` | runner: a non-blocking main agent |
| 2 | Promise.try avoids-wrap | fixed on main (e535e4b) |
| 1 | Object.freeze on a resizable-buffer TypedArray | fixed on main (e535e4b) |

- Memory cap: 5 staging/sm files fail only under it (extensions/recursion.js, RegExp/unicode-{class-,}braced.js,
  regress/regress-610026.js; String/replace-math.js is fixed in e535e4b). memcap is finding the root causes.

## Merges awaiting a post-merge measurement

Not in c21bcd82e's run; memcap measures main after ws/runtime-r's gate:
- e535e4b ws/builtins (fast-forward): immutable ArrayBuffer, ShadowRealm, Promise.try no-wrap, TA
  [[PreventExtensions]], Proxy revoke fix, GetSubstitution limit. ES-core gate 43,059 → 43,166, 0 regressions.
- 68e8bb7 ws/survey-intl 32879c42e: Intl.DateTimeFormat; intl402 non-Temporal 1,069 → 1,318 / 1,353. Gates vs
  c21bcd82e: ES-core 0, full 0 regressions. The 3 Intl-gated staging waivers pass again (waiver dropped).
- 6dab50e ws/modules2 71b2722: import defer / source-phase / text / bytes modules, AbstractModuleSource,
  ImportModuleForRealm, CoverInitializedName, B.3.2 labelled/duplicate block functions. Module-proposal files
  244 → 515 / 516; all 242 negative-syntax tests still pass, now on their real early errors (no flips). Gates:
  ES-core 0; full 1 (dst-offset-caching-6, flaky, passes alone).
- 23c2b86 ws/interp2-r 7b89bae: the previous team's interpreter commits + SNOWMAN_GC_STRESS. ES-core 0
  regressions; GC stress clean; no measurable end-to-end speedup at 4 jobs.

Runner change, measured in c21bcd82e: c21bcd8 ws/runner 43f03e9. Module tests run in place (`--prelude` /
`--epilogue`). Old vs new runner on the same binary, over the 843 module-flagged files of the full suite: 613 → 719
(+106, 0 newly failing); parse-only 733 → 733. The +106 is the runner's: the 104 self-importing tests, 1 staging
self-import, and 1 then-accidental pass (import-defer-of-missing-module-fails.js; it passes for the right reason
on temporal2's branch). Report: results branch `results/43f03e9/MERGE_REQUEST.md`.

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

- Full suite, flaky: staging/sm/Date/dst-offset-caching-{2..8}-of-8.js time out at 7–10 s under 4-job load and
  pass alone. Not a regression (builtins bisected: 8.5–9.0 s CPU at every merge since 8a5df0b, ±10% noise).
  The time goes to the interpreter: identifier resolution (ResolveBinding + DeclarativeEnvironment::find ~14%),
  evaluation ~15%, GC ~10%; Date is ~1.3%. The fix is interpreter performance (ws/interp2's commits first).

- TypedArray/prototype/copyWithin/coerced-values-{start,end}-detached(-prototype).js (3): passed by accident before
  cf35ee1, now near the 10 s timeout; interp2's speed work (waived until it lands).
