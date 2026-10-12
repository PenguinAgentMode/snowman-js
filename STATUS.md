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

- Engine commit: 2551c6978 (main)
- Parse-only (`results/2551c6978-parse.json`): 43,305 / 43,305 files; positives 38,860 / 38,860, negative-parse 4,445 / 4,445
- Milestone: **M1b** (M1a 100.0% / 100.0%, M1b 100.0% / 100.0%); full run not yet measured
- Command: `node t262/runner/milestones.mjs --parse-only results/2551c6978-parse.json`

## Waivers

Each line is a known regression, with its reason.
