# Status

| Workstream | Owner (agent) | Branch | Directories | State |
| --- | --- | --- | --- | --- |
| parser | parser | `ws/parser` | `src/parser/`, `tools/parser/` | headers merged (7a2d507); parser in progress → M1a, M1b |
| regexp | regexp | `ws/regexp` | `src/regexp/`, `src/unicode/`, `tools/unicode/` | validator merged (7a2d507); matcher (Phase B) |
| runtime | runtime | `ws/runtime` | `src/runtime/` (implements the lead's headers), `src/builtins/` except interp's files | core merged (7a2d507); M2 built-ins |
| interp | interp | `ws/interp` | `src/interp/`; `src/builtins/` function_constructor, eval, generator, async_function, async_generator, async_from_sync_iterator, promise | coroutines/environments (not pushed yet); evaluator; M2 |

Lead-owned: `main`, `src/main.cpp`, `src/engine.h`, `src/runtime/*.h` (interfaces), `INTERFACES.md`, `PLAN.md`,
`t262/`, `results/`, `STATUS.md`, `Makefile`.

## Latest result

- Engine commit:
- ES-core: passing files / 43,305
- Milestone: (`node t262/runner/milestones.mjs results/<sha>.json --parse-only results/<sha>-parse.json`)

## Waivers

Each line is a known regression, with its reason.
