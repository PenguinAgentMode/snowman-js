# Merge request: ws/runner-strict 5606282 (base main e4ac249): "use strict" on the test only, not the harness

Change (t262/runner/run.mjs, t262/README.md only): script tests get the harness (assert.js, sta.js,
doneprintHandle.js if async, includes) as `--prelude` and `print("Test262:Ran")` as `--epilogue`, exactly as module
tests already did; the entry is `"use strict";` (strict scenario only) + the test, still written next to it as
`.t262-PID-SLOT.js` so fixtures and `import()` resolve. Raw tests and `--parse-only` (entry only) are unchanged.
Preludes (one per includes + async flag, sha1-named) and the epilogue are written once per runner process, shared,
and removed at exit; the watchdog marker is the per-slot entry (or the module test), never a shared file.
INTERFACES.md section 1's last bullet ("run.mjs uses this for flags: [module] tests") needs the lead's edit.

## 1. Full suite, old runner (main 19dc921) vs new runner (5606282), SAME engine binary 19dc92105
| | old runner (rep-1) | new runner (5606282) |
| --- | --- | --- |
| passing files (of 53,616) | 53,452 | 53,465 (+13) |
| passing scenarios (of 102,994) | 102,698 | 102,712 |
| newly failing files | | 0 |
| HostScriptError scenarios | 0 | 0 |
- Newly passing: the 13 test/staging/sm/strict/* files (strict scenarios 82/95 → 95/95). Recorded as a RUNNER gain.
- Failing in both with a different error: 1, test/staging/sm/RegExp/unicode-braced.js. One scenario got past the
  `ulimit -v` out-of-memory edge; the file still fails. It's the known memory-cap pair; the pre-cache run flipped
  unicode-class-braced.js the same way.
- rep-1, rep-2, rep-3 and the lead's run of the same binary agree on every scenario (tools/lead/flaky.md), so the
  differences above come from the runner.
- `gate.mjs results/5f3218f30-full.json` on the new-runner JSONL: "baseline 5f3218f30 (full): 53587 passing files;
  branch run: 53465 passing of 53616 files; regressions: 135; expected: 0; not run: 0". The old runner on the same
  binary gets the same 135 files (Atomics wait/waitAsync/notify 109, class 22, sm/RegExp 2): engine-side
  (19dc92105 predates 5f3218f30), none from the runner.
- Per-file lists: runner-gate.md in this directory; JSONL: full.jsonl.

## 2. Speed: interleaved A/B, shard 7/50 (2,028 scenarios), same binary, 4 jobs
| | old | new | old | new |
| --- | --- | --- | --- | --- |
| before the prelude cache (f29154e), wall s | 68.9 | 73.7 | 62.8 | 85.1 |
| after the cache (5606282), wall s | 74.6 | 67.4 | 62.4 | 59.0 |
| after the cache, engine time (sum of ms), s | 292 | 264 | 244 | 230 |
Verdict: within noise of the old runner, slightly faster after the cache. The pre-cache cost was runner-side: two
synchronous temp-file writes and unlinks per scenario on the slow $TMPDIR mount, on the runner's one thread. With
the same prelude on 9p or tmpfs, the engine alone timed the same within noise. The full run's engine-time sum
(12,383 s) isn't comparable: it ran alongside a 4-job parse-only run.

## 3. Parse-only, full suite, old runner vs new runner (5606282), same binary
102,942 / 102,994 scenarios pass under both runners. Per scenario: status differs 0, missing 0. 42 failing
scenarios (all SyntaxError, the same ones under both runners) changed only their `(line:column)`, because the
harness is no longer in front of the test in parse-only: e.g. grammar-field-accessor.js "Unexpected token '$'
(233:11)" → "(19:11)". With line:column stripped, 0 differ. JSONL: parse-old.jsonl, parse-new.jsonl.
