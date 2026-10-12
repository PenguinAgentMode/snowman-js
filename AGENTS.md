# Working in snowman-js

- **Toolchain:** g++ (C++20), GNU make, node 22. No cmake, no third-party engine code, no JS parser
  libraries: the engine is ours.
- **The lead owns** `main`, `INTERFACES.md`, `t262/`, `results/` and `STATUS.md`, and is the only one who
  merges.
- **A workstream** owns the directories STATUS.md gives it, works on `ws/<name>`, and codes against
  INTERFACES.md. A change to an interface goes through the lead first.
- **Before asking to merge:**
  - `make` builds with no warnings;
  - `make slice` passes for your directories;
  - `make sample` has no file that passes on `main` and fails on your branch.
- **Never** edit `t262/` or test262 to make a test pass, and never special-case a test file.
- **Report numbers, not impressions:** "test/language/literals 812 → 1,003 of 1,041 (`make slice`)".
