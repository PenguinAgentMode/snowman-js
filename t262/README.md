# t262: the measuring stick

Runs test262 against `./engine`, sharded across machines, and says which milestone the engine reached.
It needs node 22 and nothing else. Every command runs from the repo root.

## The engine's contract

`engine [--prelude h.js]... [--epilogue e.js]... [--module] [--parse-only] [--can-block=false] file.js`
- Exit 0 when the script runs to completion.
- On an uncaught exception, print `Name: message` (for example `TypeError: x is not a function`) and exit
  non-zero.
- Provide a global `print(...)` that writes a line to stdout.
- Drain the job queue (promises) before exiting.
- Provide a global `$262` with `createRealm`, `evalScript`, `detachArrayBuffer`, `gc` and `global` (see
  test262's INTERPRETING.md).
- With `--module`, run the file as a module; imports resolve relative to it.
- With `--parse-only`, parse (including early errors) and exit 0 or report the SyntaxError; run nothing.
- With `--can-block=false`, the main agent can't suspend: `Atomics.wait` throws a TypeError (`flags:
  [CanBlockIsFalse]` tests get it).
- `--prelude` / `--epilogue` files are global Scripts run before the entry and after it completed normally;
  an error in one prints `Test262:HostScriptError <file>` first (INTERFACES.md section 1).

The runner appends `print("Test262:Ran")` after each test. A test passes only if that line is printed, so an
engine that runs nothing can't pass. A script test is composed with the harness into a temp file next to it; a
module test runs unmodified at its own path, with the harness as a prelude and the `Ran` line as an epilogue
(temp files in os.tmpdir()), so a test that imports itself gets the same module record. A
`Test262:HostScriptError` line fails the scenario whatever the test expects.

## Running

```
t262/fetch-test262.sh                                # test262 at the pinned commit
node t262/runner/run.mjs --engine ./engine --suite es-core --shard 3/8 --jobs 4 --out results/shard-3.jsonl
node t262/runner/aggregate.mjs --suite es-core --label <engine sha> --out results/<sha>.json results/shard-*.jsonl
node t262/runner/milestones.mjs results/<sha>.json --parse-only results/<sha>-parse.json
```

| Suite | Files | Scenarios |
| --- | --- | --- |
| `es-core`: language, built-ins, annexB, harness; no Temporal, no proposals | 43,305 | 82,724 |
| `full` | 53,616 | 102,994 |

- `aggregate.mjs` refuses an incomplete run (exit 2): every scenario of the suite exactly once.
- A file passes only if all its scenarios pass.
- `--baseline results/<old>.json` adds newly passing files and regressions.
- `--filter test/language/literals` runs a slice; use it for an owner's own directories before merging.
- `--parse-only` runs the M1 measurement: aggregate it separately into `results/<sha>-parse.json`.
- One full-suite run gives both numbers: aggregate its JSONL with `--suite full` into `results/<sha>-full.json` and
  with `--suite es-core --subset` (lines outside ES-core left out) into `results/<sha>.json`, then
  `milestones.mjs results/<sha>.json --full results/<sha>-full.json`. `tools/lead/measure.sh suite` does all three.
- `--jobs` defaults to 4. A watchdog SIGKILLs an engine whose resident memory passes `--mem-mb` (env
  `T262_MEM_MB`, default 700); the scenario fails with `error_name` `MemoryLimit`. Jobs x the cap must fit the machine.
  Each engine also runs under `ulimit -v` of `--vm-mb` (env `T262_VM_MB`, default 1024; 0 for none).

`results/node/es-core.json` is node itself through `hosts/node-host.mjs` (97.42% of files). A test node fails
here usually needs host support, not engine work.

## Milestones

| Rung | Target |
| --- | --- |
| M1a | parse-only: ≥ 95% of positive tests parse, ≥ 50% of negative-parse tests rejected |
| M1b | parse-only: ≥ 99.5% / ≥ 95% |
| M2 | `test/harness` ≥ 85 of 116 |
| M3 | language files with no feature tag ≥ 80%, ES-core ≥ 20% |
| M4 | ES-core ≥ 35% |
| M5 | ES-core ≥ 55% |
| M6 | ES-core ≥ 70% |
| M7 | ES-core ≥ 85% |
| M8 | ES-core ≥ 95% |

## Measuring across machines

- **After each merge to `main`**, the lead starts N shard agents, one machine each. Each one runs `make shard
  SHARD=k/N`, then commits its JSONL as `results/<sha>/shard-k.jsonl` to the `results` branch (rebase and
  retry on a push race).
- **The lead** aggregates the N files, and commits `results/<sha>.json` and the STATUS.md line to `main`.
- **Any regression** against the previous result is reverted or waived in STATUS.md.
