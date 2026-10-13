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

Merge candidate 1: 785f274ec (all of the above, main 19dc921 merged in). Gate: pending.

## GC phase timing (dst3, 8 collections, ~2,700 live cells each)
sort of all cells 16-38 ms, sweep 49-75 ms, roots (atoms ~700) 0.1 ms, stack 0.1, mark 0.2. Atom roots are
not worth changing; sort (candidate-word set instead) and fewer cells are.

## Profile notes (gprof, -pg -static to see libc)
- malloc/free/brk ~20% of samples: FDI vectors (4 per call), ValueVector per call, Binding vectors.
- GC: sort of all cells per collection; atom table traced every collection.

## Next
- FDI plan per function; stack argument buffers; GC sort; member-call fast path.
