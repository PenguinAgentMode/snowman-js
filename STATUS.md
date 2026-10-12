# Status

| Workstream | Owner (agent) | Branch | Directories | State |
| --- | --- | --- | --- | --- |
| parser | parser | `ws/parser` | `src/parser/`, `tools/parser/` | M1a, then M1b |
| regexp | regexp | `ws/regexp` | `src/regexp/`, `src/unicode/`, `tools/unicode/` | validator (M1b negatives), then matcher |
| runtime | (starting) | `ws/runtime` | `src/runtime/*.cpp`, `src/builtins/` (except interp's files) | headers from the lead first |
| interp | (starting) | `ws/interp` | `src/interp/`, interp's built-in files | after `ast.h` + runtime headers |

Lead-owned: `main`, `src/main.cpp`, `src/engine.h`, `src/runtime/*.h` (interfaces), `INTERFACES.md`, `PLAN.md`,
`t262/`, `results/`, `STATUS.md`, `Makefile`.

## Latest result

- Engine commit:
- ES-core: passing files / 43,305
- Milestone: (`node t262/runner/milestones.mjs results/<sha>.json --parse-only results/<sha>-parse.json`)

## Waivers

Each line is a known regression, with its reason.
