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
- **Merging from an older base** (the lead's rule): a branch gated on an older main may merge only if no file
  it changes was changed on main since its base. After every merge, or batch of merges, the lead has the full
  suite run on the new main (`tools/lead/measure.sh suite <baseline>`) and `gate.mjs` against the previous
  baseline. Any regression reverts the offending merge at once; it comes back re-gated on current main.
  Every published number names the main commit it was measured on.
- **Report every ~30 minutes**, even mid-task: one line to the lead (agent_send to="parent"): the branch sha
  pushed, the pass count on your directories, the next step, blockers. Silence reads as a problem.
- **Merge per feature**: when one feature (a built-in, a constructor, a class) reaches a stable pass count,
  gate it and ask to merge. Don't hold it for the rest of your area; later batches get their own gate.
- **Machines**: 4 GB enforced, shared by the build and the run: `make -j4`, `--jobs 4`. Push after every
  meaningful step; a killed machine loses everything unpushed.
- **Never** edit `t262/` or test262 to make a test pass, and never special-case a test file.
- **Report numbers, not impressions:** "test/language/literals 812 → 1,003 of 1,041 (`make slice`)".
