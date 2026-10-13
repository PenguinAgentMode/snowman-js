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
- DONE/handed off: interp2-r -> main 23c2b86; runtime-r 44d3159 gated clean (43,191, 0 regressions), the lead
  merges it after survey 5aba3f349 and the lead's own post-merge measurement (the lead now runs measurements).
- (a) ws/stacktrim 6b67bbf: coroutine budget main + 128 MB, madvise(DONTNEED) of the dead main stack before
  mapping a fresh coroutine stack; recursion.js 929 -> 657 MB, passes. ES-core gate vs results/c21bcd82e.json
  running (/tmp/stacktrim-core.jsonl); then merge request.
- (b) DONE: memory root causes in tools/lead/es-core-residue.md (ws/measure).
- (c) ws/canblock f2a0138 WIP (approved): vm().canBlock, CLI --can-block=false (INTERFACES.md §1), run.mjs
  passes it for flags: [CanBlockIsFalse], 1-line AgentCanSuspend check in builtins/atomics.cpp. Gate: old vs
  new runner, same binary, over the 9 CanBlockIs{True,False} files of the full suite (/tmp/canblock.txt),
  plus the ES-core gate. Squash the wip commit before the merge request.

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
