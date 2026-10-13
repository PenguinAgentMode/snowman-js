# memcap handoff (agent "memcap": measurer + memory/runner work)

Updated: 2026-10-13, (c) canblock gate running.

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

## In progress
- Merges queued by the lead: runtime-r 44d3159 and stacktrim 6b67bbf (both gated clean vs c21bcd82e).
- (c) ws/canblock ff86a5f (squashed): --can-block=false. Runner gate over the 9 CanBlockIs{True,False}
  files (old runner vs new, same binary): 7 -> 9, +2 CanBlockIsFalse, 0 newly failing. ES-core gate running:
  $HOME/tmp/canblock-core.jsonl vs results/c21bcd82e.json; then merge request.
- Done: memory root causes in tools/lead/es-core-residue.md (ws/measure).

## Machine / gotchas
- Worktrees: ~/repo/snowman-js (measuring checkout: keep clean, detach at the exact main sha so the label
  matches), ~/repo/main-wt, ~/repo/runner-wt, ~/repo/interp2-wt, ~/repo/runtime-wt, ~/repo/results-wt.
- /tmp is a tmpfs INSIDE the sandbox: files there count against the 4 GB. Use TMPDIR=$HOME/tmp and write
  JSONL/logs under $HOME/tmp. Private test262 copy: $HOME/t262-b (--test262 $HOME/t262-b).
- Helper scripts in $HOME/tmp: cmp.js (per-file before/after of two JSONLs), run-gate-new.mjs (runner copy
  filtered by T262_FILES), canblock.txt, modules-full.txt, htmldda.txt.
- Tool calls time out around 10 min: sleep <= 295 s per call. Background runs: run_in_background + monitor.
- Background processes live in another PID namespace: kill/pkill can't stop them. Don't start a run you
  may need to cancel.
- /proc is the outer PID namespace (the watchdog maps PPid + cmdline).
- agent_send sometimes fails with 503: retry; durable copies go on the results branch.
- Never cat JSONL; summarize with node one-liners. Full ES-core ~25-30 min, full suite ~40-50 min at 4 jobs.
