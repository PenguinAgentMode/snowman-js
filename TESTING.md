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
  run.mjs runs 4 jobs, each engine under `ulimit -v` 1 GB, and kills one above 700 MB resident (`MemoryLimit` in
  the JSONL). Run a probe that can grow (recursive generators, huge strings) outside the runner only under
  `ulimit -v 1048576` in the same shell; the engine fits in it (`fatal: out of memory` past it).

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
- **Interior pointers don't keep a cell alive.** The stack scan only recognizes a pointer to a cell's start: hold
  a raw cell pointer that stays live, or a `Rooted<T>`, across anything that can reach a safepoint (any JS call).
  `auto* x = make<T>(); auto& v = x->vec; ...` with `x` dead afterwards lets the collector free the cell
  (class.cpp's ClassElements did). ASan + `SNOWMAN_GC_STRESS=1` finds these: tools/lead/sanitizers.md.
- **Timeouts are 10 s per scenario under full parallel load.** A test that takes over ~3 s alone is at risk
  in a full run; treat it as a performance bug.
