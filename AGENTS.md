# Working in snowman-js

- **Toolchain:** g++ (C++20), GNU make, node 22. No cmake, no third-party engine code, no JS parser
  libraries: the engine is ours.
- **The lead owns** `main`, `INTERFACES.md`, `t262/`, `results/` and `STATUS.md`, and is the only one who
  merges.
- **A workstream** owns the directories STATUS.md gives it, works on `ws/<name>`, and codes against
  INTERFACES.md. A change to an interface goes through the lead first.
- **Before asking to merge** (rebased on `origin/main`):
  - `make` builds with no warnings;
  - `make slice` passes for your directories;
  - the gate, per test, not per count: run full ES-core on your branch
    (`node t262/runner/run.mjs --engine ./engine --suite es-core --jobs 4 --out /tmp/core.jsonl`, in the
    background), then `node tools/lead/gate.mjs results/<baseline>.json /tmp/core.jsonl`, where `<baseline>` is
    the baseline STATUS.md names. Every `REGRESSION` line (a file that passes on main and fails on your branch)
    is fixed or explained in the merge request (passing by accident; a timeout re-run alone with `--filter` and
    passing). A +20/−20 swap is not a clean gate.
- **Merge only a gated sha**: the lead merges exactly the commit whose full ES-core gate is clean (every
  REGRESSION line fixed or explained). A fix for a gate regression needs its own full gate before it merges; a
  slice check is for development, never for merging.
- **Merging from an older base** (the lead's rule): a branch gated on an older main may merge only if no file
  it changes was changed on main since its base. After every merge, or batch of merges, the lead has the full
  suite run on the new main (`tools/lead/measure.sh suite <baseline>`) and `gate.mjs` against the previous
  baseline. An ES-core regression reverts the offending merge at once; it comes back re-gated on current main.
  A full-suite-only regression reverts too, unless it is explained (it passed by accident, e.g. vacuously
  before a feature existed; or a timeout that passes alone) and STATUS.md waives it with an owner and a fix.
  Every published number names the main commit it was measured on.
- **No long foreground tool calls.** A foreground command must finish in under ~10 minutes. Gates, suite runs and
  long builds always run in the background (the bash tool's run_in_background, or `nohup ... > $HOME/logs/x.log
  2>&1 &` in a background call) with a completion monitor, and then you end your turn. Messages are read between
  steps; an agent stuck inside a call whose machine died hangs IN_PROGRESS for good (temporal and temporal2 were
  lost that way). At the start of every new task: `du -sh /tmp` and MemTotal − MemFree from /proc/meminfo.
- **One monitor per event.** A monitor that watches for several stages can deliver the first and never the
  second (builtins went idle that way): attach a separate monitor, printing once (`grep -m1`), for each stage
  you need to wake for.
- **Background runs get a completion monitor.** Before ending a turn with a long run going (gate, suite run,
  build) in the background, attach a `monitor` that fires when it finishes, e.g. a command source that waits
  for the process to exit and then prints the runner's summary line. An idle agent with an unwatched run never
  wakes, and from outside that looks the same as done.
- **Keep big output out of your conversation** (an agent's context has a hard limit; intl's turn died at 1M
  tokens). Redirect build, test and generator output to files; print only summaries (`tail -20`, `grep -c`,
  gate.mjs's summary line, a node one-liner over the JSONL). Never cat JSONL, results/*.json, .inc data or large
  diffs; read files in ranges.
- **Keep a handoff note**: `notes/<agent>.md` on your branch (branch, sha, state, numbers, next steps, gotchas),
  updated with every push. A dead machine or a dead context then costs one re-read, not the work.
- **Liveness**: a worker with no status line and no push for 45 minutes is presumed dead (two agents were lost
  to context overflow / hung turns). Its area is reassigned from its last pushed sha and its `notes/<agent>.md`;
  anything it never pushed is lost. A late push from a presumed-dead agent is not merged without the lead
  checking it first.
- **Report every ~30 minutes**, even mid-task: one line to the lead (agent_send to="parent"): the branch sha
  pushed, the pass count on your directories, the next step, blockers. Silence reads as a problem.
- **Merge per feature**: when one feature (a built-in, a constructor, a class) reaches a stable pass count,
  gate it and ask to merge. Don't hold it for the rest of your area; later batches get their own gate.
- **Machines**: 4 GB enforced, shared by the build and the run: `make -j4`, `--jobs 4`. Push after every
  meaningful step; a killed machine loses everything unpushed.
- **/tmp is memory.** On our sandboxes `/tmp` is a tmpfs inside the machine's 4 GB (`$HOME` is host-backed).
  `export TMPDIR=$HOME/tmp`; keep JSONL, logs, engine copies, test262 copies and worktrees under `$HOME`, and
  delete them when done. builtins' machine died with ~1.5–2 GB of files in /tmp. `MemTotal − MemFree` in
  /proc/meminfo is the machine's real usage; run.mjs's machine guard pauses at 78% and kills the largest engine
  at 86% of 4 GB.
- **Push WIP at least every 20 minutes of coding**, not only at gate points: a `wip:` commit on your own branch
  is fine (squash or reword before the merge request; the gated sha is what merges). The stack can drop a turn
  at any time (a GS restart lost survey's unpushed calendar code).
- **Never** edit `t262/` or test262 to make a test pass, and never special-case a test file.
- **Report numbers, not impressions:** "test/language/literals 812 → 1,003 of 1,041 (`make slice`)".
