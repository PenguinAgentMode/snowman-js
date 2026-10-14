# perf2 (interpreter speed, no behaviour change) — final handoff

Owner: agent "perf2" (took over from "perf", stranded). Timeboxed: batch 2 was the last perf merge (supervisor);
nothing is in flight after MR 2. perf's work landed as two gated merge requests:
- MR 1 = ws/perf2 757a0f6 (batch 1 + the C-stack fix), merged by the lead as int/perf c1214e2 -> main c2fcc37.
- MR 2 = ws/perf2b 592107d (batch 2 + BindingList kInline 2), on int/heap 050785a; the lead integrates it with
  hardening and the ASan attribute (ws/perf2b-asan 959d8b8) as int/final a6a3b0d.

## State (2026-10-14 ~00:00 UTC)
- Batch 1 (main since c2fcc37): identifier binding caches (IdentifierCache on Identifier nodes; SNOWMAN_VERIFY_IC=1
  checks every hit against a walk by name), FunctionDeclarationInstantiation plans, argument lists of <= 8 on the C
  stack (alloca'd to the count), direct o.m() calls, malloc trim threshold. Gate of 757a0f6 vs 5f3218f30: es-core 0
  / full 0 regressions, 53,587; GC_STRESS 0 newly failing; VERIFY_IC 46,588/46,588. JSONL results/757a0f6/.
- Batch 2 (MR 2): BindingList (2 Bindings inline, Binding 48 -> 32 bytes, imports in ModuleEnvironment::imports_),
  conservative roots via a hash set of in-range stack words (no sort of all cells per collection), thread_local
  atom keys (operations.cpp, function.cpp), global-environment lookups once per operation. Gate of 592107d (main's
  runner) vs 5f3218f30: es-core 0 / full 0 regressions, 53,599 / 53,616, 0 new statics; JSONL results/592107d/.
  GC_STRESS=10 vs an int/heap 050785a engine: 1,606/1,606 both, 0 newly failing. VERIFY_IC slice: 46,588/46,588,
  0 mismatches. recursion.js [sloppy] in the gate: pass, 678 MB.
- e17dad8 (batch 2 with kInline 4) failed the full gate: test/staging/sm/extensions/recursion.js [sloppy]
  MemoryLimit (702 MB peak; main 671). kInline 2: 677 MB with SNOWMAN_MEM_MB=0, 674 with the limit, 678 in the gate.
  kInline 1 would be 666 MB but loses most of batch 2's dst3 gain.
- Speed, alone, min of 3 user+sys (seconds), main 5f3218f -> batch 1 (757a0f6) -> batch 1+2 (k=4 timings; k=2 same):
  dst-offset-caching-3-of-8 6.53 -> 4.16 -> ~4.1 (vs c1214e2 4.44 in the same session); decodeURI/S15.1.3.1_A2.5_T1
  3.91 -> 3.53 -> 3.31-3.45; literals/regexp/S7.8.5_A1.1_T2 0.57 -> 0.47 -> 0.48-0.52. Machine noise is ~+-10%:
  always interleave engines in one session.
- Summed ms, same-machine A/B (full suite shard 3/20, 5,136 scenarios, main/branch/main): batch 1 +1.0% / -1.1%
  (main drift 2.1%); batch 2 592107d vs int/heap 050785a -1.1% / -3.3% (drift 2.3%). Within noise both times.
  Per-process cost dominates; the gains are in the slow files. A gate JSONL vs another session's baseline JSONL
  is useless here: this machine's speed varied by 40% between runs (757a0f6 2,372 s vs 9ff07fb 1,594 s wall).

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
- statics.mjs: 15 found, 0 new (757a0f6 and 592107d gates).
- heap.cpp forEachStackWord: no_sanitize("address") like scanRange (ws/perf2b-asan 959d8b8; no effect without ASan).

## Gate checklist (what each perf MR ran)
1. `make -j4` 0 warnings; `node tools/lead/statics.mjs` 0 new.
2. `node tools/lead/stackdepth.mjs <branch engine> <main engine>`: no probe shallower (~25 s; runs both with
   SNOWMAN_MEM_MB=0, since the soft memory limit would stop the spread probe early; --keep-mem-limit to keep it). Also look at the
   peak RSS column: heap held per recursion level matters too (recursion.js, 700 MB watchdog).
3. SNOWMAN_VERIFY_IC=1 over language/, annexB/, built-ins/{Function,eval,Atomics} (main's runner): 0 non-pass.
4. SNOWMAN_GC_STRESS=10 over built-ins/GeneratorPrototype, built-ins/Proxy, language/statements/for-await-of,
   main vs branch engine: 0 newly failing.
5. Full suite with main's runner + gate.mjs vs the newest baselines; timings alone; same-machine A/B; JSONL to
   the results branch; MR with the exact sha.

## Files
batch 1: src/interp/{environment.{h,cpp},expressions.cpp,function.{h,cpp},interpreter.h,statements.cpp},
src/runtime/heap.cpp, tools/lead/stackdepth.mjs. batch 2: src/interp/{environment.{h,cpp},expressions.cpp,
function.cpp}, src/runtime/{heap.cpp,operations.cpp} (operations.cpp approved by the lead). No interface changes.

## Ideas not done (each needs its own gate cycle; none measured)
- Per-call heap at recursion depth: environments are the bulk (~2 per sloppy call); skipping the separate
  lexical environment of a sloppy function whose body has no lexical declarations and no direct eval
  (unobservable then) would cut heap per level.
- dst3 is still ~4 s; not profiled since batch 1 (perf's gprof notes, from before, are on ws/perf): profile first.

## Tools ($HOME/bench on perf2's machine, not in the repo; each is a few lines)
- compose.mjs <test262> <rel test> <out.js> [strict]: the script run.mjs runs (harness + includes + test).
- bench.sh <engine> <file.js>...: min of 3 user+sys. gate.sh <name> <worktree>: build + full suite with main's
  runner + gate.mjs. gcstress.sh, vic.sh, ab.sh (main/branch/main on one shard), ms.mjs (summed ms), cmp.mjs
  (newly failing files), su.mjs (compare -fstack-usage frame sizes between two builds).
- Gotchas: a `cmd &` inside a foreground bash call dies when the call returns (use run_in_background). Notices
  after a notice-started turn are held until a non-notice message. -fstack-usage per function finds frame growth
  (`evaluate` is the recursive hub: anything inlined into it costs every level).
