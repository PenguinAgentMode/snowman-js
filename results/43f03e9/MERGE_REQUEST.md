# MERGE REQUEST ws/runner (memcap)

Gated sha: 43f03e9 (611df7e engine --prelude/--epilogue + 43f03e9 run.mjs/README), base main 8a5df0b. None of its
files (src/main.cpp, src/engine.h, src/interp/cli.cpp, src/interp/module.{h,cpp}, t262/runner/run.mjs,
t262/README.md) changed on main since 8a5df0b: mergeable under the older-base rule. Rebased copy (identical diff,
no conflicts): ws/runner-rebased 0ca66fa on main 6ed6999, with its own ES-core gate vs results/85210ca13.json.

- Gate 1 (supervisor): every module-flagged file of the FULL suite (843), old runner (main 8a5df0b) vs new runner,
  same binary (43f03e9). Run mode 613 -> 719: newly passing 106, newly failing 0. Parse-only 733 -> 733: no change.
- Gate 2: full suite, every file, main 8a5df0b93 vs ws/runner 43f03e9 (new runner + engine): 44,766 -> 44,868 /
  53,616 files (scenarios 85,571 -> 85,670). Newly passing 106 = gate 1's list. Newly failing 4, all timeouts of
  slow script tests: staging/sm/Date/dst-offset-caching-{5,6,7,8}-of-8.js (7-10 s each under 4-job load); re-run
  alone with --filter on ws/runner 16/16 pass, main's own engine+runner on the same re-run 8/16 time out: flaky.
- Before-error of the 106: 90 "ReferenceError: assert is not defined", 13 no error text, 1 "fnGlobalObject is not
  defined", 1 "$DONE is not defined", 1 "asyncTest called without async flag". They are all 104 files of
  src/interp/module_test_exclusions.txt (now obsolete), staging/sm/module/module-export-name-star.js (self-import),
  and the accidental pass below.
- ACCIDENTAL PASS (temporal2's expected list): test/language/import/import-defer/errors/resolution-error/
  import-defer-of-missing-module-fails.js: the harness as a global Script gives asyncTest its global $DONE; the
  parser rejects `import defer` in main_FIXTURE.js, so import() rejects as the test expects.
- README line (in the commit): a module test runs unmodified at its own path, with the harness as a prelude and the
  Ran line as an epilogue (temp files in os.tmpdir()), so a test that imports itself gets the same module record; a
  Test262:HostScriptError line fails the scenario whatever the test expects.
- Behaviour: a module whose evaluation is still pending after the job queue drains now exits 1 ("Error: module
  evaluation did not complete"; it exited 0). Without the new flags, scripts and modules run as before.
- Per-file diff with error text: runner-gate.md; JSONL: modules-old-runner.jsonl, modules-new-runner.jsonl,
  suite.jsonl (this directory).
