# perf2 (interpreter speed, no behaviour change)

Owner: agent "perf2" (took over from "perf", stranded). Branch ws/perf2, worktree $HOME/wt/perf2.
perf's work in two merge requests, each gated on its own:
- MR 1: batch 1 (ws/perf 19dc921..e7f8922) rebased onto main 5f3218f: identifier slot caches (SNOWMAN_VERIFY_IC=1
  self-check), FDI plans, stack argument lists, direct o.m() calls, malloc trim threshold.
- MR 2: batch 2 (ws/perf 98a5e21 + 42ff096) on top: inline binding storage, candidate-set stack scan, cached atoms,
  global env lookups once per op.

## State
- 2026-10-13 ~19:50 UTC: MR 1 rebased (49e206c + notes); only conflict was heap.cpp includes (<atomic> from step
  4, <malloc.h> from batch 1; both kept). expressions.cpp auto-merged: step 4 made identifierName's atom cache
  thread_local, batch 1 doesn't touch it. 13154f3: make 0 warnings; SNOWMAN_VERIFY_IC=1 over language/, annexB/,
  built-ins/{Function,eval,Atomics} (main's runner): 46,588 / 46,588 scenarios pass, 0 cache mismatches.
- Rebased onto main 9c2ead8 (results only; src identical to 13154f3). Gate baseline: results/5f3218f30{,-full}.json
  (ES-core 43,305 / 43,305). Next: full-suite gate, GC_STRESS slices, timings.

## Thread safety (agents are threads since step 4)
- Batch 1/2 cache on AST nodes (Identifier::interpCache, FDI plans per function node, literal atoms): safe only
  while every AST is used by one thread. Today each agent parses its own source (agent.cpp StartAgent ->
  parser::parseProgram on the agent thread) and no process-wide parsed AST exists. **Sharing parsed ASTs across
  threads (a parse cache, shared builtin JS) would break these caches** (atoms are per-thread heap cells; the
  caches are unsynchronised). builtins reviewed batch 2: no new mutable statics; per-thread caches fine.
- heap.cpp kMallocTuned: namespace-scope const, mallopt at static init (before any thread); process-wide by design.
- Atom key caches (operations.cpp OperationKeys, function.cpp FunctionKeys): thread_local, as agent-design.md asks.
- statics.mjs: 15 found, 0 new (on the batch 1 + 2 trial).

## Gate plan (each MR)
1. `make -j4` warning-free. SNOWMAN_VERIFY_IC=1 on an ES-core slice (language/expressions, statements,
   class decorators, built-ins/Function): 0 mismatches.
2. Full suite with main's runner + gate.mjs vs the newest STATUS.md baselines (19dc92105, or 5f3218f once measured);
   SNOWMAN_GC_STRESS=10 over GeneratorPrototype, Proxy, for-await-of (0 newly failing); statics.mjs.
3. Speed: summed ms vs the baseline JSONL; dst-offset-caching-3-of-8, decodeURI/S15.1.3.1_A2.5_T1,
   literals/regexp/S7.8.5_A1.1_T2 alone (min of 3 user+sys). JSONL to the results branch. MR with the exact sha.

## Files
batch 1: src/interp/{environment.{h,cpp},expressions.cpp,function.{h,cpp},interpreter.h,statements.cpp},
src/runtime/heap.cpp. batch 2 adds src/runtime/operations.cpp (cached atoms; approved by the lead).

## Tools ($HOME/bench, not in the repo)
- compose.mjs <test262> <rel test> <out.js> [strict]: the script run.mjs runs (harness + includes + test).
- bench.sh <engine> <file.js>...: min of 3 user+sys, run alone (nothing else running).
- Gotcha: a `cmd &` inside a foreground bash call dies when the call returns; use run_in_background.
