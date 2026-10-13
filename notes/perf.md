# perf (interpreter speed, no behaviour change)

Branch ws/perf. Owner: agent "perf". Bench: min of 3 user+sys, alone, files composed by $HOME/bench/compose.mjs
(harness + includes + test). Baseline main 1c12abb: dst-offset-caching-3-of-8 6.66 s, decodeURI
S15.1.3.1_A2.5_T1 4.02 s, literals/regexp/S7.8.5_A1.1_T2 0.53 s.

## Done (on ws/perf, not gated yet)
1. Identifier binding caches (IdentifierCache on Identifier::interpCache, interpreter.h): hops + slot, or hops
   to the global environment. Valid through static declarative records only; eval-declared vars (deletable
   bindings) and deletes mark a record dynamic. References carry the slot (GetValue/PutValue fast path).
   SNOWMAN_VERIFY_IC=1 checks every hit against a walk by name. dst3 6.66 -> 6.26.

## Profile notes (gprof, -pg -static to see libc)
- malloc/free/brk ~20% of samples: FDI vectors (4 per call), ValueVector per call, Binding vectors.
- GC: sort of all cells per collection; atom table traced every collection.

## Next
- FDI plan per function; stack argument buffers; GC sort; member-call fast path.
