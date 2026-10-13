# Testing in practice (owned by the lead)

Lessons from this project's own incidents. t262/ is the measuring stick and stays as it is.

## Running

- **One runner per test262 checkout.** run.mjs writes `.t262-PID-SLOT.*` temp files inside `t262/test262/`, and
  another runner listing scenarios at that moment can crash on them (ENOENT). For a second concurrent run, use a
  private copy and pass it: `cp -r t262/test262 /tmp/t262-b && node t262/runner/run.mjs --test262 /tmp/t262-b ...`
  or a separate `git worktree` with its own `make test262`.
- **Long commands go in the background.** A foreground tool call times out after several minutes, and its
  process keeps running in its own PID namespace, where pkill can't reach it. A full ES-core run takes about
  25 minutes on 16 jobs. Use `make slice FILTER=...` for loops, and run full runs with run_in_background.
- **`cmd &` inside a tool call dies when the call returns.** Use run_in_background, never a trailing `&`.
- **Don't rebuild `./engine` while a run uses it.** Build in a worktree, or wait.
- **Memory is the machine's limit, not the test's.** Machines have 4 GB, enforced by killing the whole machine;
  run.mjs runs 4 jobs and kills an engine above 700 MB resident (`MemoryLimit` in the JSONL). Run a probe that
  can grow (recursive generators, huge strings) outside the runner only under `ulimit -v` in the same shell.

## Baselines

- Every full run on main is published: `results/<sha>.json` on main (aggregate, passing files), and
  `results/<sha>/full.jsonl` on the `results` branch (per scenario, with the error text). Diff your branch
  against it instead of re-running main:
  `git fetch origin results && git show origin/results:results/<sha>/full.jsonl > /tmp/main.jsonl`.
- Node's baseline through the same runner is `results/node/es-core.json`. `node tools/lead/nodegap.mjs
  results/<sha>.json` splits our failures into real gaps (node passes) and data points (node fails too).

## Traps

- **Passing by accident.** A test can pass because an earlier step throws the error it expects (copyWithin
  passed while `$262.detachArrayBuffer` threw a TypeError; `await using` tests passed while nothing was disposed).
  When a fix makes a passing test fail, check whether it only ever passed by accident and say so in the merge
  request.
- **The GC is barely exercised by test262** (most tests never reach the collection budget). Run
  `SNOWMAN_GC_STRESS=N ./engine test.js` (collect every N safepoints) on anything that touches cells.
- **Timeouts are 10 s per scenario under full parallel load.** A test that takes over ~3 s alone is at risk
  in a full run; treat it as a performance bug.
