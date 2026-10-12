# Status

| Workstream | Owner (agent) | Branch | Directories | State |
| --- | --- | --- | --- | --- |
| parser | parser | `ws/parser` | `src/parser/`, `tools/parser/` | M1a + M1b reached (a43fced; measured on main 2551c6978) — done |
| regexp | regexp | `ws/regexp` | `src/regexp/`, `src/unicode/`, `tools/unicode/` | validator + matcher merged (1761872); RegExp built-in tests |
| runtime | runtime | `ws/runtime` | `src/runtime/` (implements the lead's headers), `src/builtins/` except interp's files | M2 built-ins + Date, buffers, TypedArrays, Atomics merged (18fad18); RegExp built-in (ws/runtime-regexp) |
| interp | interp | `ws/interp` | `src/interp/`; `src/builtins/` function_constructor, eval, generator, async_function, async_generator, async_from_sync_iterator, promise | evaluator merged (bc8b128, links; untested: no parser yet); modules; M2 |

Lead-owned: `main`, `src/main.cpp`, `src/engine.h`, `src/runtime/*.h` (interfaces), `INTERFACES.md`, `PLAN.md`,
`t262/`, `results/`, `STATUS.md`, `Makefile`.

## Latest result

- Engine commit: eff4d1fa9 (main)
- ES-core (`results/eff4d1fa9.json`): **38,993 / 43,305 files (90.04%)**; scenarios 74,645 / 82,724; test/harness
  110 / 116; es5_language 4,353 / 4,538 (95.9%)
- Parse-only (`results/2551c6978-parse.json`): 43,305 / 43,305
- Milestone: **M7** (M1a, M1b, M2, M3, M4, M5, M6, M7 reached; M8 ≥ 95% not yet)
- Command: `node t262/runner/milestones.mjs results/eff4d1fa9.json --parse-only results/2551c6978-parse.json`
- Biggest gaps: RegExp built-in not merged (1,687 files), modules (397 + import 10; interp's module commits
  unpushed), $262.detachArrayBuffer (~250 TypedArray/DataView/Atomics files), Iterator 230, String 188,
  Atomics 177 (incl. $262.agent), DisposableStack/AsyncDisposableStack/SuppressedError 219, Uint8Array base64 62.

## Waivers

Each line is a known regression, with its reason.
