# builtins: handoff note

Agent "builtins" (workstream owner under the lead). Branch for the current batch: `ws/builtins5` (from main 46746ad).

## Merged (all with a clean full ES-core gate)

- a87b211 (main 5b81747): Promise.allKeyed / allSettledKeyed 89/89, Error.prototype.stack 35/35, Iterator.prototype.join 18/18.
- d0423a1 (main 8e85e96): legacy RegExp statics 26/26 (+ staging regress-642975); Function.prototype.toString writes a built-in
  name that isn't an IdentifierName as a computed string (`function get ["$&"]() { [native code] }`).
- e535e4b (main, fast-forward): immutable-arraybuffer 61/66 (5 import-bytes left: need `type: "bytes"` in interp/module.cpp,
  module owner); ShadowRealm 63/64 (3 importValue files pass by accident until the loader hook is used); Promise.try no-wrap;
  TypedArray [[PreventExtensions]] (IsTypedArrayFixedLength); Proxy target/handler captured before the trap lookup;
  GetSubstitution 2^30 length limit before allocating. Gate: 43,166 / 43,305, 0 regressions vs results/85210ca13.json.

## Interfaces I added (additive, approved)

- runtime/arraybuffer.h: ArrayBufferObject::isImmutable/setImmutable, AllocateImmutableArrayBuffer, IsImmutableBuffer.
- runtime/typedarray.h: AccessMode + ValidateTypedArray(o, AccessMode); TypedArrayObject::PreventExtensions;
  IsTypedArrayFixedLength.
- runtime/realm.h: Intrinsic::ShadowRealm, ShadowRealmPrototype.

## FINAL STATE (parked, main b3dc641: ES-core 43,305 / 43,305, full 53,612 / 53,616)

Merged, all with clean full gates: Promise.allKeyed/allSettledKeyed, Error.prototype.stack, Iterator.prototype.join,
legacy RegExp statics, immutable ArrayBuffer, ShadowRealm (+ importValue), Promise.try / TypedArray
[[PreventExtensions]] / Proxy capture / GetSubstitution limit, nonextensible-applies-to-private, the Temporal calendars
(framework + gregory, groups 2-4, chinese/dangi reference years), $262.agent steps 1-4 (agent.{h,cpp}: threads on 16 MB
coroutine stacks, mailboxes, process-wide waiter lists, _Exit once agents exist), the thread_local conversion and its
audit, tools/lead/statics.mjs (run by gate.mjs), the soft memory limit (heap.cpp: RSS from statm vs SNOWMAN_MEM_MB,
default 640; relief = malloc_trim + dead stack, no forced GC; then RangeError("out of memory") at the next CheckStack).

Known gaps (leave unless asked):
- 9 chinese intl402/Temporal files: ICU 78.3 differs from the official calendar (CNY 2027/2030, the 1987 leap month).
  If ever fixed: astronomical computation in gen-calendars.mjs, cross-checked against ICU; no third-party data.
- 2 staging Annex B `arguments` files contradict ES-core's block-decl-func-skip-arguments.js.
- Soft memory limit edges: allocations in bursts between the 64K-cell samples can overshoot it (recursion.js peaks at
  675 MB on perf2, under the 700 MB watchdog); parser AST memory (regress-610026) isn't sampled and still ends with the
  parser's fatal out-of-memory under perf2. perf2b's GC scan without sortedCells_ lowers the cost of a GC at depth.
- 6 DTF "Ramadan" dateStyle tests (intl side), ZonedDateTime/links.js CST6CDT (tz side).

## Monitor pitfall (seen 16:54)

- A poll monitor that prints twice (`while ! grep -q STAGE1 log; do sleep 20; done; grep ...; while ! grep -q DONE log;
  do sleep 30; done; tail ...`) delivered only the first print: the gate's DONE line was never notified and I went idle.
  `tail -n0 -F log | grep --line-buffered` monitors also exited early (code 2) here. What works: one monitor per event,
  a single loop that prints exactly once at the end (`while ! grep -q DONE log; do sleep 30; done; tail -2 log`).

## How I work

- Worktrees: ~/repo/snowman-js (main checkout, owns t262/test262), ~/repo/wt2, ~/repo/wt3, ~/repo/wt4 (bisect builds). The
  worktrees symlink t262/test262 to the main checkout's; never run two runners on one checkout: private sparse copies in /tmp
  (`~/tools/mksub.sh <dest> <feature-regex>`), `~/tools/feat.mjs <regex> <jsonl>` for per-feature counts,
  `~/tools/slicegate.sh <jsonl>` for a slice against results/8a5df0b93-full.json (needs /tmp/full-base.json and /tmp/gate.mjs).
- Gates: copy the engine to /tmp/engine-X first, run the full ES-core in the background with a completion monitor, one gate at a
  time (4 jobs max per machine), `node tools/lead/gate.mjs <baseline> /tmp/coreN.jsonl`. A stopped runner leaves `.t262-*`
  temp files in the checkout: `/usr/bin/find -L t262/test262/ -name ".t262-*" -delete` before the next run.
- Gotchas: busybox grep (no --include), busybox date (no %N); TESTING.md's SNOWMAN_GC_STRESS isn't in this tree.
