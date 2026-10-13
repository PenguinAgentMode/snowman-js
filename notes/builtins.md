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

## Current batch: $262.agent

- ws/builtins5 d55e14b on main ec34f4f: $262.agent step 1 (host.cpp; conflict with $262.AbstractModuleSource resolved,
  both kept) + notes + importValue through ImportModuleForRealm (ShadowRealm full slice 64/64). Full ES-core gate of
  d55e14b running (vs results/c21bcd82e.json). Step 1 alone (6ce8f3e) was gated clean: 43,169, +3.
- ws/builtins6 dd70aa8: nonextensible-applies-to-private, gated clean (43,166, 0 regressions; full slice +3), sent alone.
- d55e14b gated clean (43,169) and sent. Step 2 (thread_local) on ws/builtins7, rebased on main 258ff44 (tip 7ab3d0e),
  gate running (/tmp/core10.jsonl, engine /tmp/engine-tls; main engine for the speed run: /tmp/engine-main258):
  88850c4 runtime (vm, heap, atom table, emptyString, index strings, length key), 37fc0f0 builtins (lastIndex key,
  Math.random state; seeded per thread by the Math installer), aa57585 interp (gCurrent, gStackPool thread_local;
  gStackBytes std::atomic with an atomic reserve-then-check; gModuleAsyncEvaluationCount), 63fb221 parser (gChunk,
  stackLimit; parser.h `extern thread_local` = interface change to list), 96ad528 intl caches, aeade2f temporal tz cache.
  vm.h / heap.h unchanged. Next: gate it in quiet conditions, then the speed comparison vs main 7a345df (sum of ms over
  the ES-core run, and CPU-heavy files alone).
- Baselines: results/c21bcd82e.json (ES-core 43,163), results/c21bcd82e-full.json (full 50,887).

## Earlier batch

1. importValue: replace the stub in src/builtins/shadowrealm.cpp (ShadowRealmImportValue) with
   `ImportModuleForRealm(evalRealm, specifierString, innerCapability);` + `#include "interp/module.h"`, once temporal2's
   module work (hook ec0f102) is on main (the lead says when). Prototyped on e535e4b + ec0f102 (local branch proto-import in
   ~/repo/wt2): importValue 20/20 scenarios, and the 3 formerly accidental files reject through the real paths (missing export
   -> ExportGetter TypeError; syntax error / throwing / missing module -> onRejected TypeError).
2. Date slowdown: DONE (no fix in my files). staging/sm/Date/dst-offset-caching-{2..8}-of-8 take 8.0-9.6 s CPU alone on
   every commit from 8a5df0b to main (8a5df0b 8.6 s, 00dc9d9 8.6, 9f1e7e5 8.5, 9a1a42b 8.9, 797c7f7 9.0): no regression in
   the range, they were always near the 10 s timeout and time out under load. gprof (main 46746ad, -pg): Date path ~1.3% of
   6.95 s (2.97 M Date constructions + getTimezoneOffset + setTime); the rest is the interpreter: ResolveBinding 7.8% (27 M
   calls), DeclarativeEnvironment::find 4.2% (160 M), evaluateStatement/evaluateMember/calls ~20%,
   FunctionDeclarationInstantiation 2.3%, GC mark ~10%. A per-second localtime_r cache in date.cpp gave no measurable gain:
   dropped. Routed to the lead as interpreter performance (src/interp/, interp2's unmerged perf work).

## Left with others (agreed with the lead)

- sm/regress/regress-602621, sm/lexical-environment/block-scoped-functions-annex-b-arguments: contradict ES-core
  annexB/language/function-code/block-decl-func-skip-arguments.js; leave.
- block-scoped-functions-annex-b-label / -deprecated-redecl (parser/scope.cpp exitScope: labelled block functions never become
  Annex B candidates; `count == 1` excludes duplicates) and regress-610026 (AST memory): parser, temporal2's queue.
- sm/extensions/recursion: interp/coroutine.h kStackBytesBudget (736 MB) is above the 700 MB RSS watchdog: memcap.
- Intl-dependent staging failures since the intl merge (sm/Proxy/revoked-get-function-realm-typeerror, sm/String/internalUsage,
  sm/extensions/quote-string-for-nul-character): intl's.

## How I work

- Worktrees: ~/repo/snowman-js (main checkout, owns t262/test262), ~/repo/wt2, ~/repo/wt3, ~/repo/wt4 (bisect builds). The
  worktrees symlink t262/test262 to the main checkout's; never run two runners on one checkout: private sparse copies in /tmp
  (`~/tools/mksub.sh <dest> <feature-regex>`), `~/tools/feat.mjs <regex> <jsonl>` for per-feature counts,
  `~/tools/slicegate.sh <jsonl>` for a slice against results/8a5df0b93-full.json (needs /tmp/full-base.json and /tmp/gate.mjs).
- Gates: copy the engine to /tmp/engine-X first, run the full ES-core in the background with a completion monitor, one gate at a
  time (4 jobs max per machine), `node tools/lead/gate.mjs <baseline> /tmp/coreN.jsonl`. A stopped runner leaves `.t262-*`
  temp files in the checkout: `/usr/bin/find -L t262/test262/ -name ".t262-*" -delete` before the next run.
- Gotchas: busybox grep (no --include), busybox date (no %N); TESTING.md's SNOWMAN_GC_STRESS isn't in this tree.
