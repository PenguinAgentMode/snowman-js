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

## Current batch

1. importValue: replace the stub in src/builtins/shadowrealm.cpp (ShadowRealmImportValue) with
   `ImportModuleForRealm(evalRealm, specifierString, innerCapability);` + `#include "interp/module.h"`, once temporal2's
   module work (hook ec0f102) is on main (the lead says when). Prototyped on e535e4b + ec0f102 (local branch proto-import in
   ~/repo/wt2): importValue 20/20 scenarios, and the 3 formerly accidental files reject through the real paths (missing export
   -> ExportGetter TypeError; syntax error / throwing / missing module -> onRejected TypeError).
2. Date slowdown: staging/sm/Date/dst-offset-caching-{2..8}-of-8 take 8-10 s alone on main (797c7f7) and were faster on
   8a5df0b93. Bisect candidates: temporal 00dc9d9, temporal2 9f1e7e5, intl 9a1a42b. Fix it if it's in builtins/date.cpp's
   local-time path; else report cause + fix to the lead (time_zone.cpp = temporal2, intl/* = intl/survey).

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
