# memcap handoff (agent "memcap": measurer + memory/runner work)

Updated: 2026-10-13, after the c21bcd8 measurement.

## Done (merged)
- ws/memcap (main 5ff85e8): 480 MB main stack, 256 KB coroutine stacks + budgets, parser nesting guard,
  run.mjs RSS watchdog 700 MB + ulimit -v 1024 MB, 4 jobs.
- ws/runner 43f03e9 (main c21bcd8): module tests in place (--prelude/--epilogue). Gate write-up:
  results branch results/43f03e9/{MERGE_REQUEST.md,runner-gate.md}.

## Measurements (ws/measure; JSONL on the results branch)
- main 8a5df0b93: full 44,766 / 53,616 (83.49%), ES-core 43,059 / 43,305. results/8a5df0b93{,-full}.json.
- main c21bcd82e: full 50,887 / 53,616 (94.91%), ES-core 43,163 / 43,305 (99.67%). Full-suite gate vs
  8a5df0b93-full: 3 real regressions from intl 9a1a42b (Intl exists, Intl.DateTimeFormat undefined:
  staging/sm/{Proxy/revoked-get-function-realm-typeerror,String/internalUsage,extensions/
  quote-string-for-nul-character}.js) + flaky staging/sm/Date/dst-offset-caching-* timeouts (7-10 s each).

## In progress (lead's queue, in order)
1. DONE ws/interp2-r 7b89bae -> main 23c2b86 (gate vs c21bcd82e: 0 regressions, 43,166; GC stress=10 clean).
   ws/runtime-r: 416792a gated clean on base e535e4b (43,191, 0 regressions) but overlaps main in
   src/interp/{expressions,host}.cpp, so rebased on main ec34f4f as 44d3159 (host.cpp: kept both lines);
   its ES-core gate vs results/c21bcd82e.json runs at /tmp/runtime2-core.jsonl. Then merge request.
2. After runtime-r merges: post-merge `tools/lead/measure.sh suite results/c21bcd82e.json` on main (detach
   at the exact sha), gate.mjs ES-core vs c21bcd82e and full vs c21bcd82e-full, clusters.mjs vs
   c21bcd82e-full, publish (ws/measure + results branch), send milestones verbatim.
3. Then a fresh-start brief from the lead for the next area. Pending items that brief may cover:
   - kStackBytesBudget vs the 700 MB watchdog (staging/sm/extensions/recursion.js): part 1 (infinite
     plain recursion) leaves ~480 MB of main stack resident; part 2 starts one 256 KB coroutine per level.
     A 128 MB coroutine budget alone may not suffice: measure; option madvise(DONTNEED) of dead main stack.
   - Memory root causes, to confirm by measurement: unicode-{class-,}braced build a 16M-char pattern
     ("0".repeat(2**24)) through eval/new RegExp, so several 32 MB UTF-16 copies + M_TRIM_THRESHOLD 1 GB keep
     address space growing; regress-610026 evals ~2M then ~4M "{}" blocks (~380 B/block: AST + scope + env).
   - tools/lead/es-core-residue.md (ws/measure 4f999b1) is written; section 4 (memory) still to add.

## Machine / gotchas
- Worktrees: ~/repo/snowman-js (measuring checkout: keep clean, detach at the exact main sha so the label
  matches), ~/repo/main-wt, ~/repo/runner-wt, ~/repo/interp2-wt, ~/repo/runtime-wt, ~/repo/results-wt.
- /tmp/t262-b: private test262 copy for runs outside the measuring checkout (--test262 /tmp/t262-b).
- Tool calls time out around 10 min: sleep <= 295 s per call. Background runs: run_in_background + monitor.
- Background processes live in another PID namespace: kill/pkill can't stop them. Don't start a run you
  may need to cancel.
- /proc is the outer PID namespace (the watchdog maps PPid + cmdline).
- agent_send sometimes fails with 503: retry; durable copies go on the results branch.
- Never cat JSONL; summarize with node one-liners. Full ES-core ~25-30 min, full suite ~40-50 min at 4 jobs.
