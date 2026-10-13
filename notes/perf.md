# perf (interpreter speed, no behaviour change)

Branch ws/perf. Owner: agent "perf". Bench: min of 3 user+sys, alone, files composed by $HOME/bench/compose.mjs
(harness + includes + test). Baseline main 1c12abb: dst-offset-caching-3-of-8 6.66 s, decodeURI
S15.1.3.1_A2.5_T1 4.02 s, literals/regexp/S7.8.5_A1.1_T2 0.53 s.

## Done (on ws/perf, not gated yet)
1. Identifier binding caches (IdentifierCache on Identifier::interpCache, interpreter.h): hops + slot, or hops
   to the global environment. Valid through static declarative records only; eval-declared vars (deletable
   bindings) and deletes mark a record dynamic. References carry the slot (GetValue/PutValue fast path).
   SNOWMAN_VERIFY_IC=1 checks every hit against a walk by name. dst3 6.66 -> 6.26.

2. FunctionDeclarationInstantiation plans (function.cpp): for a simple parameter list without duplicates or
   expressions and no sloppy direct eval, the first call's binding list is kept per function node and copied
   on later calls (params/arguments/functions filled in). 6.26 -> 5.45.
3. Argument lists of <= 8 non-spread arguments in a stack array (the C stack is scanned); evaluate() of a call
   without References for `o.m(...)`; member bases evaluated directly outside chains. 5.45 -> 4.30.
4. heap.cpp: mallopt(M_TRIM_THRESHOLD 256 MB, M_TOP_PAD 16 MB): no brk shrink/grow after each sweep. -> ~3.5.

Merge candidate 1: e7f8922 (= 785f274ec + notes; main 19dc921 merged in). Verify-mode ES-core run of the cache
commit cc4d891: gate vs 317949e37 0 regressions, 0 mismatches. Full gate of e7f8922: running.

## Batch 2 (wip on ws/perf after e7f8922, not gated)
5. Inline binding storage: DeclarativeEnvironment::BindingList (4 inline), Binding 48 -> 32 bytes (module
   import targets moved to ModuleEnvironment::imports_). dst3 -12% and peak RSS 307 -> 287 MB (under load).
6. heap.cpp: conservative roots from a hash set of in-range stack words (no sort of all cells per
   collection; falls back to the sort when the stacks hold more words than half the cells).
7. Cached atoms for "prototype"/"length"/"constructor" (operations.cpp) and function-creation keys.
8. Global environment: one declarative-record lookup per operation; ObjectEnvironment's ordinary-object check
   once at construction; SetMutableBinding via PropertyMap::setWritableValue (one lookup).

## Measuring
- The machine has ~4 cores of CPU quota: benchmarks during a suite run are slowed and slow the run.
- ES-core summed ms is ~50 ms x 82,724 scenarios, mostly per-process cost (engine startup does ~no CPU work:
  realm creation is 1.7 ms); hot-loop speedups move the sum little, the slow files a lot.
- Tools in $HOME/bench (not in the repo): compose.mjs, bench.sh (min of 3), gcstress.sh, cmp.mjs, ms.mjs.

## GC phase timing (dst3, 8 collections, ~2,700 live cells each)
sort of all cells 16-38 ms, sweep 49-75 ms, roots (atoms ~700) 0.1 ms, stack 0.1, mark 0.2. Atom roots are
not worth changing; sort (candidate-word set instead) and fewer cells are.

## Profile notes (gprof, -pg -static to see libc)
- malloc/free/brk ~20% of samples: FDI vectors (4 per call), ValueVector per call, Binding vectors.
- GC: sort of all cells per collection; atom table traced every collection.

## Next
- FDI plan per function; stack argument buffers; GC sort; member-call fast path.
