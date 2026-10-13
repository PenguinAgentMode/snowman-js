# Plan (owned by the project lead)

## Architecture

- **Parser** (`src/parser/`): hand-written recursive descent over UTF-16 source, producing an ESTree-shaped C++
  AST (`src/parser/ast.h`) annotated with the static semantics the evaluator needs (strictness, var/lexical
  declarations, hoisted functions, parameter shape, `arguments`/direct-eval use). All early errors are
  reported at parse time. `--dump-ast` prints acorn-compatible ESTree JSON so the AST can be diffed.
- **Runtime** (`src/runtime/`): a tagged `Value`; UTF-16 strings that may hold lone surrogates; objects with
  the essential internal methods as virtuals so exotic objects override them; a tracing GC (precise inside
  the heap, conservative over the C stacks), allocate-only at first; realms with a %Intrinsic% table.
- **Interpreter** (`src/interp/`): a tree-walking evaluator that follows the spec's runtime semantics
  directly. Generators and async functions run on stackful coroutines (ucontext, mmap'd stacks), so the
  evaluator stays a plain recursive function. Promise jobs drain before exit.
- **Built-ins** (`src/builtins/`): one file per built-in, self-registering, declared with name, length and
  attributes.
- **RegExp** (`src/regexp/`): our own validator and backtracking matcher; Unicode tables in `src/unicode/`,
  generated from node 22's Unicode 17.0 data (the version test262's generated tests use).

## Order

1. **M1a → M1b** (parse-only): parser; regexp validator for the RegExp early errors. In parallel, the runtime
   core and the interpreter start against `ast.h` and the runtime headers.
2. **M2** (`test/harness` ≥ 85/116): interpreter + runtime + the built-ins the harness uses (assert.js,
   sta.js, propertyHelper.js, compareArray.js, deepEqual.js, asyncHelpers.js: Error types, Object, Function,
   Array, String, Symbol, Promise, async functions, JSON).
3. **M3** (no-feature language ≥ 80%, ES-core ≥ 20%): language completeness in the interpreter; built-ins split
   across workstreams by file (see STATUS.md).

## Measuring

- After each merge to `main`, the lead runs the measurement for the current phase: parse-only for M1
  (`results/<sha>-parse.json`), full ES-core from M2 on (`results/<sha>.json`), locally or with shard agents.
- `node t262/runner/milestones.mjs results/<sha>.json --parse-only results/<sha>-parse.json` is the score.

## Phase 2: the full suite (`--suite full`, 53,616 files)

ES-core reached M8 (97.99%). The goal now is the full suite's pass rate without regressing ES-core. Full = ES-core
(43,305) + 10,311 more files, per `listScenarios`:

| Cluster | Files | Owner | Notes |
| --- | --- | --- | --- |
| Memory safety (gate for everything) | — | memcap | watchdog merged 8e39780; stack budgets + `ulimit -v` next |
| test/built-ins/Temporal | 4,605 | temporal | ISO calendar, UTC/offset zones; new `src/builtins/temporal/` |
| test/intl402/Temporal | 2,029 | later | needs IANA tz data + non-ISO calendars: after Temporal + Intl |
| test/intl402 (other) | ~1,330 | after survey | Intl constructors, option processing, minimal locale data |
| test/staging | ~1,480 | after survey | mostly staging/sm (SpiderMonkey's non262 imports) |
| Proposal-tagged files in language/, built-ins/, annexB | ~800 | after survey | language/expressions 399, language/import 113, Promise 89, ShadowRealm 64, Error 35, ArrayBuffer 29 |
| ES-core long tail | ~870 | later | 35 tail-call tests pass only via the 1 GB main stack (proper tail calls needed to shrink it) |

Ranked by files unlocked per unit of work once the survey (`tools/lead/survey-full.md`) has the causes.

Gates: a workstream runs full ES-core on its rebased branch (~30 min at 4 jobs) and shows no regression against
the latest `results/<sha>.json` before asking to merge; the lead re-measures the full suite on main after merges.
