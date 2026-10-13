# stability (workstream handoff)

Branch `ws/stability` from main 19dc921 (label 19dc92105). Measurement only, no engine code.

## Plan / state
1. Built 19dc92105 (`make -j4 engine`, 0 warnings); sha256 of ./engine in $HOME/logs/engine.sha256.
2. Full suite x3 on that one binary, chained in one background shell: $HOME/logs/rep-{1,2,3}.{jsonl,log}
   (`node t262/runner/run.mjs --engine ./engine --suite full --jobs 4 --out ...`), "REP_DONE" appended to each log.
3. Compare with the lead's results/19dc92105/suite.jsonl: `node tools/lead/flaky.mjs --label 19dc92105
   --out tools/lead/flaky.md lead=<suite.jsonl> rep-1=... rep-2=... rep-3=...` (classes: memory, timeout, crash, other).
4. Parse-only ES-core after the repeats (not concurrently, so it doesn't skew their timings), aggregate.mjs to
   results/19dc92105-parse.json, milestones.mjs for M1a/M1b.
5. Push flaky.md + parse.json here, rep JSONLs to the results branch as results/19dc92105/rep-{1,2,3}.jsonl.
