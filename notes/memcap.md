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
1. Gate ws/interp2-r (local + push: 3 commits rebased on main 0c92d79-ish; interp2's Promise.try commit
   dropped, main already has the same fix) and ws/runtime-r (351e794 rebased + my IsHTMLDDA host hook
   + typeof commit). Each: make warning-free, full ES-core vs results/c21bcd82e.json, GC stress spot-check
   (SNOWMAN_GC_STRESS=N exists only with interp2's commit).
2. kStackBytesBudget under the 700 MB watchdog for staging/sm/extensions/recursion.js. Its part 1 (infinite
   plain recursion) leaves ~480 MB of main stack resident; part 2 starts one 256 KB coroutine per level.
   Lowering the coroutine budget to 128 MB may not suffice: measure, consider madvise(DONTNEED) of the dead
   main-stack pages.
3. tools/lead/es-core-residue.md on ws/measure: group the 142 failing ES-core files by cause (nodegap:
   111+3 Atomics, 16+9 annexB, 2 Promise, 1 Object); $262.agent design estimate. Per-agent state found:
   vm(), heap(), atom table (runtime/string.cpp), coroutine globals (gCurrent, gStackPool, gStackBytes),
   parser gChunk, gScriptDirectory, waitAsync WaiterLists.
4. Root causes of the 5 memory-hungry staging tests (peak RSS: recursion.js 932 MB; unicode-{class-,}braced
   444 MB but >1 GB VM; regress-610026 2.3 GB; replace-math 1.3 GB, fixed by builtins' GetSubstitution).

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
