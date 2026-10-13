# Status

| Workstream | Owner (agent) | Branch | Directories | State |
| --- | --- | --- | --- | --- |
| parser | (closed) | `ws/parser` | `src/parser/`, `tools/parser/` | M1a + M1b reached (a43fced; main 2551c6978); agent closed |
| regexp | regexp | `ws/regexp` | `src/regexp/`, `src/unicode/`, `tools/unicode/`, iterator_zip/iterator_chunking/atomics_wait_async built-ins | matcher + Iterator.zip/chunks merged (69c2ac0); Atomics.waitAsync |
| runtime | runtime | `ws/runtime` | `src/runtime/`, `src/builtins/` except others' files | RegExp built-in merged (7a69924); built-in failures, perf |
| interp | (closed; work recovered at e639ee9 and merged) | `ws/interp` | — | merged |
| interp2 | interp2 | `ws/interp2` | `src/interp/` except modules; interp's built-in files; disposable_stack, async_disposable_stack, suppressed_error, array_from_async | detachArrayBuffer merged (cf35ee1); speed + GC on, explicit resource management (incl. module-level `using`), Array.fromAsync, language failures, perf |
| modules | (closed) | `ws/modules` | — | merged 69cadee (TLA rejection order); module-level `using` → interp2 |

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

## Waivers

Each line is a known regression, with its reason.

- TypedArray/prototype/copyWithin/coerced-values-{start,end}-detached(-prototype).js (3): passed by accident before
  cf35ee1, now near the 10 s timeout; interp2's speed work (waived until it lands).
