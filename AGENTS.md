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
- **Never** edit `t262/` or test262 to make a test pass, and never special-case a test file.
- **Report numbers, not impressions:** "test/language/literals 812 → 1,003 of 1,041 (`make slice`)".
