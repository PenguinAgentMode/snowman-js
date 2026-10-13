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

## C stack per JS call (the margin)
The 4 test/language/statements/for/tco-{const,let,lhs,var}-body.js [strict] need 100,000 nested calls: more than
432 MB of the 480 MB main stack on main (<10% headroom). **No change may lower the recursion depth.** Check every
batch in seconds before a gate: `node tools/lead/stackdepth.mjs <branch engine> <main engine>` (exit 1 on a drop).
- 9ff07fb (batch 1 as perf left it) failed it: plain calls -9.9% (EvaluateCall's fixed Value argList[8], evaluateNew's
  inlined into evaluate, evaluateCallValue's member path in every call's frame); the lead's integration gate lost
  the 4 tco tests (RangeError). Fixed in the next commit: argument arrays by alloca sized to the count in a frame of
  their own (call) or in evaluate's (new); evaluateMethodCallValue, throwNullishPropertyRead, evaluateReferenceValue
  and evaluateArray/Object/TaggedTemplate/MetaProperty out of line (evaluate's frame 592 -> 544 bytes + alloca).
- Main 5f3218f under ulimit -v 1 GiB: plain 182,508 (532 MB peak RSS), method 182,508 (529), args-8 182,508 (622),
  args-9 182,508 (634), spread 182,508 (681), getter 203,840 (535), callback 150,920 (624), construct 257,306 (602).
- Fixed batch 1: plain 216,492 (+18.6%), method 239,629 (+31.3%), args-8 205,172 (+12.4%), args-9 209,276 (+14.7%),
  spread 209,276 (+14.7%), getter 236,025 (+15.8%), callback 186,853 (+23.8%), construct 263,793 (+2.5%).
  tco-const-body peak RSS 519 MB (main) -> 474 MB.
- Deeper also means more heap at the overflow: peak RSS at the RangeError +15..20 MB on args-9/spread/callback
  (spread 699 MB, the runner's watchdog is 700). A spread of *elements* retains iterator garbage per level and
  runs out of memory before the stack on main too (not a probe).
- Speed of the fixed batch 1 (alone, min of 3 user+sys): dst3 6.53 -> 4.16 s, decodeURI 3.91 -> 3.53, regexp
  0.57 -> 0.47 (main 5f3218f -> fix; 9ff07fb was 4.33 / 3.70 / 0.49).

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
