# Status

| Workstream | Owner (agent) | Branch | Directories | State |
| --- | --- | --- | --- | --- |
| memcap | (presumed dead 14:19 UTC: 47 min without a push or status, interrupt stays queued) → calendars to builtins; its ws/canblock ff86a5f rebased and gated by the lead (ws/canblock-r 7205b4d) | `ws/memcap`, `ws/runner`, `ws/measure` | `t262/`, `src/main.cpp`, `src/interp/cli.cpp` for the runner change | runner change gated: module tests 613 → 719 (+106, 0 newly failing); full-suite script check running; then post-merge measurement, then ws/interp2 + ws/runtime. Earlier: merged 5ff85e8: 480 MB main stack, 256 KB coroutine stacks + budgets (RangeError), parser depth guard, `ulimit -v` 1024 MB; 0 regressions. Open: dropped suspended generators keep their stack until exit (cap ~1,000; needs 2 heap.h methods); no CheckStack on nested array/object literals (SIGSEGV past ~400 levels in a generator; no test hits it) |
| temporal | (presumed dead since ~09:00 UTC: messages and interrupts stay queued; nothing pushed after 7526397) → calendars to survey | `ws/temporal` | `src/builtins/temporal/` core (temporal.h, parser, options, calendar) + Duration, Plain*; Intrinsics in realm.h; Date.prototype.toTemporalInstant | merged 00dc9d9 (built-ins/Temporal 3,047 alone; 4,605 / 4,605 with ws/temporal2). Its files are now survey's (calendar*.cpp, CalendarISOToDate in temporal.h) and temporal2's (time zones) |
| temporal2 | (presumed dead 13:47 UTC: no push for 63 min, interrupt stays queued; last push ws/calendars 67c5b59) → calendars to memcap | `ws/temporal2`, `ws/modules2`, `ws/calendars` | Temporal tz layer (merged 9f1e7e5); now `src/parser/`, `src/interp/module.*`, `builtins/abstract_module_source.cpp`, realm.h AbstractModuleSource, `$262.AbstractModuleSource` | module proposals: import defer/source/text/bytes (516 files incl. 242 negative), ShadowRealm loader hook, CoverInitializedName; later parser B.3.2 hoisting + AST size |
| intl + Temporal calendars | survey lost in a GS restart (12:15 UTC); successor couldn't be spawned (no free hosts). Calendars → temporal2; the pushed toLocaleString batch ws/survey-intl 5aba3f349 → gated by memcap; non-Gregorian DTF output waits | `ws/survey-intl`, `ws/calendars` | `src/builtins/intl/`, `tools/intl/`; Intl intrinsics in realm.h; toLocale* in number/bigint/string/array/typedarray/date.cpp | merged 9a1a42b (intl402 non-Temporal 1,069 / 1,353). DateTimeFormat batch ws/intl-dev ccdf619 (1,317 / 1,353; DTF 210/245) to rebase and gate; it turns the 3 Intl-gated staging waivers green |
| builtins | builtins | `ws/builtins` | promise/error/iterator/regexp/arraybuffer/typedarray/dataview/atomics/uint8array_base64 built-ins; additive `runtime/arraybuffer.h`, `runtime/typedarray.h` | merged 5b81747 (allKeyed 89/89, Error.stack 35/35, Iterator.join 18/18); gating legacy RegExp (26/26) and immutable ArrayBuffer (61/66). Next: ShadowRealm (importValue's loader hook after temporal2), 3 ES-core bugs, staging engine bugs + Annex B |
| survey | (reopened for intl, above) | `ws/survey` | | report merged d44aba3 |
| parser, regexp, interp, modules | (closed) | | | merged before M8 |
| runtime, interp2 | (closed; previous team) | `ws/runtime` (+1), `ws/interp2` (+4) | | unmerged perf/IsHTMLDDA commits; to gate on ES-core before merging |

Lead-owned: `main`, `src/main.cpp`, `src/engine.h`, `src/runtime/*.h` (interfaces), `INTERFACES.md`, `PLAN.md`,
`t262/`, `results/`, `STATUS.md`, `Makefile`.

## Latest result

Post-merge run: **main 317949e37** (`tools/lead/measure.sh suite results/a1a2a5fc3.json`, 4 jobs, 1,323 s, run by
the lead; `results/317949e37{,-full}.json`; JSONL `results/317949e37/suite.jsonl` on the results branch).

- milestones.mjs: highest **M8**; **ES-core: 43,196 / 43,305 files (99.75%)**; **full suite: 53,414 / 53,616 files
  (99.62%)**; scenarios 102,622 / 102,994.
- vs a1a2a5fc3: ES-core +0, 0 regressions; full +1,184 (calendar groups 2 + 3), 0 regressions.
- Command: `node t262/runner/milestones.mjs results/317949e37.json --full results/317949e37-full.json`

| Cluster | Passing (317949e37) | Files | % | vs a1a2a5fc3 |
| --- | ---: | ---: | ---: | ---: |
| ES-core | 43,196 | 43,305 | 99.75 | +0 |
| built-ins/Temporal | 4,605 | 4,605 | 100.00 | +0 |
| intl402/Temporal | 2,009 | 2,029 | 99.01 | +1,184 |
| intl402 (other) | 1,336 | 1,353 | 98.74 | +0 |
| staging | 1,449 | 1,483 | 97.71 | +0 |
| proposals (in ES-core dirs) | 819 | 841 | 97.38 | +0 |
| **full suite** | **53,414** | **53,616** | **99.62** | **+1,184** |

Earlier: a1a2a5fc3 full 52,230 (97.41%); 29a14dfdd 51,639 (96.31%); 3edca9940 51,607 (96.25%); c21bcd82e 50,887 (94.91%); 8a5df0b93 44,766 (83.49%).
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

Not in 317949e37's run:
- ws/calendars-ref addd9a2 (chinese/dangi PlainMonthDay reference years, calendar.cpp only). Gate vs 317949e37:
  ES-core 0, full 0 regressions; full 53,414 → 53,418.
- 6203fc5 ws/agent3 d995052 ($262.agent step 3: SharedBlock + std::atomic_ref Atomics; arraybuffer.h additive).
  Gate vs 317949e37: ES-core 0, full 0 regressions (neutral with one agent).

Runner change, measured in c21bcd82e: c21bcd8 ws/runner 43f03e9. Module tests run in place (`--prelude` /
`--epilogue`). Old vs new runner on the same binary, over the 843 module-flagged files of the full suite: 613 → 719
(+106, 0 newly failing); parse-only 733 → 733. The +106 is the runner's: the 104 self-importing tests, 1 staging
self-import, and 1 then-accidental pass (import-defer-of-missing-module-fails.js; it passes for the right reason
on temporal2's branch). Report: results branch `results/43f03e9/MERGE_REQUEST.md`.

## Decisions

- 2026-10-13 16:33: the stack stopped the dead agents' machines (memcap, survey, temporal, intl, temporal2); every
  commit from them was already on origin. New workers (fresh contexts): chinese (astronomical Chinese/Dangi
  tables, 9 files), decorators2 (decorators, 22 + 2 staging), staging (staging triage, 34), intlfmt (non-Gregorian
  DTF output, Ramadan, tz link). builtins keeps $262.agent (109 ES-core). Plan each area to finish or hand off
  within ~5 h (a 6-hour run cap exists).

- 2026-10-13 14:19: memcap presumed dead (liveness rule). builtins, the last live worker, owns the whole calendar area
  (first merge: temporal2's framework 67c5b59 + builtins' chinese/dangi/islamic-umalqura). The lead gates orphaned
  pushed branches itself (memcap's CanBlockIsFalse: 9 CanBlock-flagged files, 18 scenarios, 14 → 18, 0 newly
  failing, a runner + CLI change).
- 2026-10-13 13:47: temporal2 presumed dead (liveness rule). Its calendar WIP (ws/calendars 67c5b59: CalendarSystem,
  CalendarDate/CalendarISOToDate, generic non-ISO ops, gregory) and notes/temporal2.md pass to memcap. Decorators
  (ws/decorators fb2d287, parser WIP) are parked without an owner. Live pool: builtins, memcap.
- 2026-10-13: survey's last turn was lost in a GS restart; spawning a successor failed ("no hosts match the
  requirements"). The live pool is 3: builtins, temporal2, memcap. Asked the supervisor to free the dead agents'
  machines.
- 2026-10-13, supervisor: intl and temporal are presumed dead (listed, unreachable, probably still holding
  machines). The live pool is 4 workers (builtins, temporal2, survey, memcap); no replacements until the user
  allows more machines. Liveness rule in AGENTS.md (45 minutes).

- 2026-10-13, supervisor: no ICU. Locale data (intl) and tz data (temporal2) are generated from node by checked-in
  scripts. Each .inc records the script, the command and node's versions, and regenerating it is byte-identical.
- 2026-10-13, supervisor: module tests run in place (INTERFACES.md §1, preludes/epilogues), so the 104
  self-importing module tests can pass. Recorded as a runner change, with a per-test before/after on every module
  test in the full suite. A prelude/epilogue error never satisfies a negative test.
- Expected flips: when `import defer` / `import source` land, most of the 242 proposal negative-syntax tests that
  pass today only because the parser rejects all of `import.defer`/`import.source` may change status. That
  branch's gate lists them separately as expected, with each one checked against its specific early error.

## Gate baseline

- ES-core: `results/317949e37.json` (43,196 passing files); full suite: `results/317949e37-full.json` (53,414).
  Check: `node tools/lead/gate.mjs results/317949e37.json <jsonl>` and `... results/317949e37-full.json <jsonl>`
  (AGENTS.md). Gates already running against c21bcd82e stay valid (a subset of this baseline).

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

## Known gaps (not regressions)

- intl402/Temporal, 9 Chinese-calendar files (daysInYear 4, monthCode M06L vs M07 5): our tables are generated from
  node's ICU 78.3, which differs from the official (HKO) Chinese calendar in a few years (CNY 2027 02-07 vs 02-06,
  2030 02-02 vs 02-03, the 1987 leap month after month 7 vs 6); test262 follows the official calendar. If it's
  ever fixed, it'll be an astronomical new-moon/solar-term computation cross-checked against ICU (no third-party
  data). Deferred: 9 files vs $262.agent's 112.
- intl402/Temporal/ZonedDateTime/links.js: tzdb 2026d made CST6CDT etc. Zones again; we follow node's data.

## Waivers

Each line is a known regression, with its reason.

- Full suite, flaky: staging/sm/Date/dst-offset-caching-{2..8}-of-8.js time out at 7–10 s under 4-job load and
  pass alone. Not a regression (builtins bisected: 8.5–9.0 s CPU at every merge since 8a5df0b, ±10% noise).
  The time goes to the interpreter: identifier resolution (ResolveBinding + DeclarativeEnvironment::find ~14%),
  evaluation ~15%, GC ~10%; Date is ~1.3%. The fix is interpreter performance (ws/interp2's commits first).

- TypedArray/prototype/copyWithin/coerced-values-{start,end}-detached(-prototype).js (3): passed by accident before
  cf35ee1, now near the 10 s timeout; interp2's speed work (waived until it lands).
