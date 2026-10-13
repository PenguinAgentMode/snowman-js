# Survey: where the full test262 suite fails beyond ES-core

Engine `7e957a05a` (main at survey time), test262 `2e0a56762`. Measured by the `survey` agent.

- Per-scenario data, on the `results` branch:
  - `results/7e957a05a/extra.jsonl`: the extra set (full minus ES-core), 20,270 scenarios.
  - `results/7e957a05a/es-core-f294bec1b-failures.jsonl`: f294bec1b's 872 failing ES-core files, re-run on 7e957a05a.
- How it was run: private copy `/tmp/t262x` of the checkout. Every file that `listScenarios(orig, 'es-core')` lists was deleted from it; harness and fixtures were kept. A transitive check found no extra test whose imports reach a deleted file. Run: `run.mjs --suite full --jobs 1`, with the engine under `ulimit -v 2900000`.
  - `listScenarios(copy, 'full')` = 20,270 scenarios / 10,311 files = full (102,994 / 53,616) minus ES-core (82,724 / 43,305). `aggregate.mjs` reports it complete.
  - Wall time: 364 s.

## 1. Headline

| Area | Files | Pass | Fail | Main cause |
| --- | ---: | ---: | ---: | --- |
| `built-ins/Temporal` | 4,605 | 0 | 4,605 | `Temporal` is not defined (4,604 of 4,605) |
| `intl402/Temporal` | 2,029 | 0 | 2,029 | `Temporal` is not defined (all); also needs Intl and non-ISO calendars |
| `intl402` (other) | 1,353 | 25 | 1,328 | `Intl` is not defined (1,311 of 1,328) |
| proposal-tagged files in `language/`, `built-ins/`, `annexB/` | 841 | 252 | 589 | Syntax not parsed, or built-in missing |
| `staging` | 1,483 | 1,432 | 51 | Mixed (section 5) |
| **extra total** | **10,311** | **1,709** | **8,602** | |

Two warnings:
- **Proposal passes are mostly accidental.** 242 of the 252 proposal passes are negative-syntax tests, which pass only because the parser rejects all of `import.defer(…)` / `import.source(…)` with "The only valid meta property for import is 'import.meta'". When that syntax lands, the specific early errors must land with it, or these tests regress. The positive proposal files pass 10 / 599.
- **staging passes 96.6%**, far better than expected: `staging/sm` is mostly already-standard behaviour.

## 2. Ranked clusters (extra set)

Work estimates: S = days for one agent; M = about a week; L = a multi-week workstream.

| # | Cluster | Files | Pass | Fail | Cause | Work | src touched |
| ---: | --- | ---: | ---: | ---: | --- | :---: | --- |
| 1 | Temporal (built-ins), plus `Date.prototype.toTemporalInstant` (8), `staging/Temporal` (2), `staging/sm/Date/to-temporal-instant` (1) | 4,616 | 0 | 4,616 | No `Temporal`. Tests are ISO-calendar with UTC/offset zones only; just 2 files name an IANA zone. Needs ISO 8601 parsing, BigInt epoch-ns math, Duration balancing/rounding, roughly 250–300 API members. | L (split by class, see §3) | new `src/builtins/temporal*` (or `src/temporal/`); intrinsics in `src/runtime/realm.{h,cpp}` + `builtins.cpp` (lead-owned headers); `builtins/date.cpp` |
| 2 | intl402/Temporal | 2,029 | 0 | 2,029 | Needs #1 plus Intl. 1,776 files use non-ISO calendars (hebrew, chinese, islamic-*, japanese, …), 105 name IANA zones, 108 format via toLocaleString/DateTimeFormat. | L+ (data: calendar algorithms + tzdb) | `src/temporal/` calendar layer, `src/intl/` |
| 3 | Intl (intl402 without Temporal) | 1,353 | 25 | 1,328 | No `Intl`. About 600 files need only API plumbing, ~190 BCP 47 tag tables, ~465 locale data (§4). | L; data decision first | new `src/intl/`; `builtins/{string,number,bigint,date,array,typedarray}.cpp` (toLocale*, localeCompare); realm intrinsics |
| 4 | import-defer | 252 | 94* | 158 | Parser lacks `import defer * as ns from` (92× "Unexpected token '*'") and `import.defer()` (63× meta-property error). Then deferred namespace semantics (evaluate on first property access). | M | `parser/statements.cpp`, `parser/expressions.cpp`, `parser/ast.h`, `interp/module.{h,cpp}`, `interp/expressions.cpp` |
| 5 | source-phase-imports (+ module-source) | 253 | 149* | 104 | `import.source()` (90× meta-property error), `import source x from` not parsed, `%AbstractModuleSource%` missing (8 files). For JS modules the source phase is a SyntaxError, so no new representation is needed. | S–M | same files as #4, plus a new `builtins/abstract_module_source.cpp` |
| 6 | await-dictionary (`Promise.allKeyed` / `allSettledKeyed`) | 89 | 2 | 87 | Not implemented. 51 are async tests that end with no error line. | S | `builtins/promise.cpp` |
| 7 | immutable-arraybuffer (+ import-bytes 5) | 66 | 2 | 64 | Missing `transferToImmutable`, `sliceToImmutable`, the `immutable` getter, and write rejection in TypedArray/DataView/Atomics. 18 fail in the harness: "no arg factories match include immutable". | M | `runtime/arraybuffer.{h,cpp}` (lead-owned .h), `runtime/typedarray.cpp`, `builtins/{arraybuffer,typedarray,dataview,atomics,uint8array_base64}.cpp` |
| 8 | ShadowRealm | 64 | 0 | 64 | Not defined. | M–L | new `builtins/shadowrealm.cpp`, `runtime/realm.*`, `interp/module.cpp` (importValue) |
| 9 | error-stack-accessor | 35 | 1 | 34 | No `Error.prototype.stack` getter/setter (32× reading 'get'/'set' of undefined descriptor). | S | `builtins/error.cpp` |
| 10 | legacy-regexp (annexB/built-ins/RegExp 26, staging 1) | 27 | 0 | 27 | No `RegExp.$1`–`$9` / `input` / `lastMatch` / …; the tests include receiver checks for subclass and cross-realm. | S–M | `builtins/regexp.cpp` (+ realm state) |
| 11 | decorators (language 24, staging 3) | 27 | 3 | 24 | `@` token and `accessor` keyword not parsed. Then the class-element decorator semantics. | L for full semantics (only 24 tests) | `parser/lexer.cpp`, `parser/statements.cpp`, `parser/expressions.cpp`, `interp/class.cpp` |
| 12 | Iterator.prototype.join | 18 | 2 | 16 | Not implemented. | S | `builtins/iterator.cpp` |
| 13 | import-text | 6 | 0 | 6 | `with { type: 'text' }` module type. | S (after #4/#5 parser work) | `interp/module.cpp` |
| 14 | nonextensible-applies-to-private (+ staging/sm/PrivateName 1) | 3 | 0 | 3 | Private field/method add on a non-extensible object must throw TypeError. | S | `interp/class.cpp` |
| 15 | staging non-proposal remainder | 42 | — | 42 | 14 runner artefacts, 16 host/non-standard, 8 engine bugs, 4 Annex B edge cases (§5). | S (bugs) | various (§5) |

\* Passing, but almost all accidentally (see the warning in §1).

Clusters that share files can't be staffed independently: #4, #5 and #13 (one "module proposals" workstream: parser + module.cpp); #1 and #2; #3 and #2. #8 also touches `interp/module.cpp` and realm, so either sequence it after #4/#5 or have the same owner do it.

## 3. Temporal by class (built-ins/Temporal and intl402/Temporal)

| Class | built-ins files | test dirs (prototype / other) | intl402 files |
| --- | ---: | --- | ---: |
| ZonedDateTime | 901 | 51 / 21 | 583 |
| PlainDateTime | 773 | 41 / 30 | 483 |
| PlainDate | 652 | 34 / 20 | 493 |
| Duration | 540 | 26 / 29 | 21 |
| PlainYearMonth | 509 | 24 / 23 | 327 |
| PlainTime | 493 | 20 / 20 | 12 |
| Instant | 467 | 17 / 14 | 17 |
| PlainMonthDay | 199 | 13 / 22 | 90 |
| Now | 66 | 9 | 3 |
| Temporal (toStringTag, keys, prop-desc…) | 5 | — | — |
| **total** | **4,605** (0 pass) | | **2,029** (0 pass) |

- 1,477 of the 4,605 files include `temporalHelpers.js` (it already loads in our engine: the tests get as far as touching `Temporal`). One file fails differently: `prop-desc.js`, where `typeof Temporal` is "undefined".
- Dominant errors (files):
  - `ReferenceError: Temporal is not defined`: about 90% of files.
  - The rest are `Expected a TypeError/RangeError but got a ReferenceError`, which has the same cause.
- Suggested split, in dependency order:
  1. Duration + Instant + Now (BigInt math, rounding).
  2. PlainDate / PlainTime / PlainDateTime (ISO calendar).
  3. PlainYearMonth / PlainMonthDay.
  4. ZonedDateTime (offset zones; tzdb only for intl402).

## 4. intl402 (without Temporal) by constructor and by what it needs

This is a content heuristic, approximate (±10%):
- **locale data**: the test calls an output-producing method (format*, compare, select, segment, of, toLocale*, localeCompare, maximize/minimize, Locale info getters, supportedValuesOf).
- **non-en**: the test also names a locale other than en / en-US / en-GB / und.
- **BCP 47**: no output call, but the test uses Locale / getCanonicalLocales / supportedLocalesOf / canonicalisation.
- **plumbing**: everything else (property shape, branding, option reading order and validation, resolvedOptions defaults).

| Constructor | Files | Pass | Plumbing only | BCP 47 tag tables | Locale data, en/default | Locale data, non-en | Also needs Temporal |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| NumberFormat | 251 | 0 | 104 | 7 | 37 | 103 | 0 |
| DateTimeFormat | 245 | 0 | 103 | 11 | 52 | 14 | 65 |
| Locale | 190 | 0 | 76 | 59 | 23 | 32 | 0 |
| DurationFormat | 110 | 0 | 41 | 8 | 53 | 2 | 6 |
| ListFormat | 81 | 0 | 39 | 11 | 14 | 17 | 0 |
| RelativeTimeFormat | 80 | 0 | 44 | 11 | 10 | 15 | 0 |
| Segmenter | 79 | 0 | 45 | 13 | 20 | 1 | 0 |
| Intl (getCanonicalLocales, supportedValuesOf, …) | 66 | 0 | 3 | 38 | 23 | 2 | 0 |
| Collator | 65 | 0 | 52 | 7 | 3 | 3 | 0 |
| DisplayNames | 57 | 0 | 50 | 0 | 2 | 5 | 0 |
| PluralRules | 53 | 0 | 38 | 6 | 8 | 1 | 0 |
| top-level intl402/*.js | 22 | 0 | 4 | 18 | 0 | 0 | 0 |
| String (localeCompare, toLocale{Upper,Lower}Case) | 19 | 7 | 0 | 0 | 5 | 7 | 0 |
| Date (toLocale*String) | 12 | 8 | 2 | 0 | 2 | 0 | 0 |
| BigInt / Number / Array / TypedArray toLocaleString | 21 | 10 | 0 | 0 | 7 | 4 | 0 |
| FallbackSymbol | 2 | 0 | 2 | 0 | 0 | 0 | 0 |
| **total** | **1,353** | **25** | **603** | **189** | **259** | **206** | **71** |

- About 600 files (45%) fail only on missing `Intl` plumbing.
- About 190 more need BCP 47 parsing plus CLDR alias / likely-subtag tables (small, but real data).
- About 465 need formatting/collation/segmentation data: 259 work with en/root data, 206 need other locales.
- The 25 passes are `length`/`name`/`builtin` checks on the existing `toLocale*` / `localeCompare` methods, plus `this`-value checks.
- **Decision for the lead before staffing:** AGENTS.md rules out third-party *engine* code. Does that also rule out ICU / ICU4X? If yes, the reachable target is plumbing + BCP 47 + en-only data, about 1,070 of 1,353 files, plus Intl-aware toLocale*. With ICU most of the 1,353 are reachable, and it also unlocks intl402/Temporal calendars.

## 5. staging: the 51 failing files by cause

`staging/sm` passes 1,364 / 1,406. Its 42 failures are counted below together with the 9 other staging failures (`staging/built-ins` 3, `decorators` 2, `source-phase-imports` 2, `Temporal` 2).

| Cause | Files | Detail | Kind |
| --- | ---: | --- | --- |
| Strict-mode prefix reaches the harness | 13 | `sm/strict/*`: the runner prepends `"use strict"` to harness + test, so `non262-strict-shell.js`'s "lenient" `eval` is strict as well. Each fails only in the strict scenario. | runner/harness (not engine) |
| Self-import module | 1 | `sm/module/module-export-name-star.js` imports itself (same ceiling as ES-core's 104). | runner |
| `function.caller` / `f.arguments` legacy extension | 12 | `sm/regress/regress-577648-{1,2}`, `regress-586482-{1..4}`, `regress-584355`; `sm/extensions/{arguments-property-access-in-function,censor-strict-caller,function-caller-skips-eval-frames,function-properties}`; `sm/Function/function-caller-restrictions`. Non-standard (needs call-stack tracking). | host/non-standard |
| `$262.IsHTMLDDA` | 2 | `sm/expressions/nullish-coalescing`, `sm/generators/yield-star-throw-htmldda` (shared with ES-core's 25, §6) | host feature |
| `Date.parse` fallback formats | 2 | `sm/Date/non-iso` ("1997-3-8 1:1:1"), `sm/Date/two-digit-years` ("1/2/99"). Implementation-defined. | host-defined, `builtins/date.cpp` |
| Proposals | 9 | Temporal 3, decorators 2, source-phase 2, nonextensible-private 1, legacy RegExp statics 1 (`sm/statements/regress-642975`, via `RegExp.lastMatch`) | proposals (§2) |
| Annex B block-function edge cases | 4 | `sm/regress/regress-602621` and `sm/lexical-environment/block-scoped-functions-annex-b-{arguments,label}` (block `function arguments(){}` and labelled function declarations); `…-deprecated-redecl` | engine (`interp/declarations.cpp`): node passes all 4. Per B.3.2.1, evaluating a block `function arguments(){}` still sets the function-level `arguments` binding; probably related to 2d134b3 ("block functions named arguments are not hoisted") |
| **Engine bugs** | **8** | `staging/built-ins/{Object/preventExtensions,Object/seal,Reflect/preventExtensions}/*-variable-length-typed-arrays` (3): integrity ops on length-tracking TAs (same root cause as ES-core's Object.freeze gap); `sm/Proxy/revoke-as-side-effect`: **SIGSEGV** when a handler trap lookup revokes the proxy; `sm/String/replace-math`: **abort on std::bad_alloc** where a catchable RangeError (string too long) is expected; `sm/regress/regress-610026`: **abort on bad_alloc** in `eval` of 2^21 `{}` blocks (AST memory under a 2.9 GB cap); `sm/destructuring/bug1396261`: missing early error for `[{a = 0}.x] = []` (CoverInitializedName); `sm/extensions/recursion`: "fatal: cannot map a coroutine stack" (the generator-stack memory issue already being fixed) | engine |

Engine-bug files:
- `runtime/typedarray.cpp`
- `runtime/proxy.cpp`
- `runtime/string.cpp` / `builtins/string.cpp` (length check before allocating)
- `parser/ast.*` (node size)
- `parser/expressions.cpp`
- `interp/coroutine.cpp`

Summary by kind:
- harness/runner reasons: 14.
- host or non-standard features: 16.
- proposals: 9.
- real engine gaps: 12 (8 bugs + 4 Annex B).

## 6. ES-core: what remains

f294bec1b's 872 failing files (from `origin/results:results/f294bec1b/full.jsonl`) were re-run on 7e957a05a. **626 now pass and 246 still fail.** Assuming no regressions among the other 42,433 files (I didn't re-run them), ES-core is about **43,059 / 43,305 (99.43%)**.

`nodegap.mjs results/f294bec1b.json` on the baseline: 432 failures were real gaps (node passes) and 440 were failures node shares. The 626 fixed since then:

| Directory | Failing at f294bec1b | Pass at 7e957a05a | Still fail |
| --- | ---: | ---: | ---: |
| built-ins/Iterator | 221 | 221 | 0 |
| built-ins/Atomics | 162 | 48 | 114 |
| built-ins/AsyncDisposableStack | 104 | 104 | 0 |
| built-ins/Array (fromAsync) | 95 | 95 | 0 |
| language/module-code | 91 | 1 | 90 |
| language/statements (await using / using) | 62 | 62 | 0 |
| built-ins/RegExp (property-escapes timeouts) | 23 | 23† | 0 |
| annexB/language | 22 | 6 | 16 |
| built-ins/Function, JSON | 42 | 42 | 0 |
| language/expressions | 17 | 4 | 13 |
| annexB/built-ins | 9 | 0 | 9 |
| other (AsyncIteratorPrototype 9, DisposableStack 5, arguments-object 2, TypedArray 2, decodeURI* 2, Promise 2, Object 1, Proxy 1) | 24 | 20 | 4 |

† These ran alone at `--jobs 1`; the slowest takes 3.2 s (4 of the 50 RegExp/decodeURI files take over 3 s). At full parallel load they may still be near the 10 s timeout.

The 246 still failing, by cause:

| Cause | Files | Node fails too | Fix lives in |
| --- | ---: | ---: | --- |
| Self-import module tests (runner composes `.t262-*.mjs`; `src/interp/module_test_exclusions.txt`) | 104 | 104 | runner (t262/, lead): not engine |
| `$262.agent` missing (Atomics wait / notify / waitAsync multi-agent tests: `getReport` of undefined) | 112 | 109 | `interp/host.cpp`, plus real threads and a shared SAB for the full set (L). node's agent stub alone passes 3 (waitAsync `true-for-timeout` ×2, `…resolves-to-timed-out`). |
| `Atomics.wait` cannot-suspend ([[CanBlock]] false for the main agent) | 2 | 2 | host choice |
| `$262.IsHTMLDDA` missing (annexB `*emulates-undefined*`) | 25 | 25 | `interp/host.cpp` + ToBoolean (`runtime/conversions.cpp:67`), typeof, IsLooselyEqual (`runtime/operations.cpp`, `interp/operators.cpp`): S–M, needs an object flag in `runtime/object.h` (lead interface) |
| `Promise.try` avoids-wrap (2026 test; node 22 predates it) | 2 | 2 | `builtins/promise.cpp`: S |
| `Object.freeze` on a resizable-buffer-backed TypedArray | 1 | 0 | `runtime/typedarray.cpp`: S (same fix as the 3 staging integrity tests) |
| **total** | **246** | **242** | |

Real engine gaps among them: 3 files (Promise.try 2, Object.freeze 1), plus IsHTMLDDA (25) and `$262.agent` (112) if the lead decides the host should provide those.

## 7. Reproduce

```
cp -r t262/test262 /tmp/t262x   # then delete listScenarios(orig,'es-core') files from the copy
node t262/runner/run.mjs --engine /tmp/capped-engine --test262 /tmp/t262x --suite full --jobs 1 --out /tmp/extra.jsonl
node t262/runner/aggregate.mjs --test262 /tmp/t262x --suite full --label 7e957a05a-extra --out /tmp/extra.json /tmp/extra.jsonl
git show origin/results:results/7e957a05a/extra.jsonl > /tmp/extra.jsonl   # or reuse the published one
```

## Appendix A. Proposal files by `features:` tag (589 failing of 841)

This is the combined tag set of each file. Errors are the first failing scenario's `error_name: message` (digits shown as N), counted per file. "(no error line)" means an async test that ended without reporting.

| Tag(s) | Files | Pass | Fail | Top errors (files) |
| --- | ---: | ---: | ---: | --- |
| import-defer | 251 | 94 | 157 | 92× SyntaxError: Unexpected token '*'; 63× SyntaxError: The only valid meta property for import is 'import.meta'; 2× Test262Error: asyncTest called without async flag |
| await-dictionary | 89 | 2 | 87 | 51× (no error line): ; 13× TypeError: Cannot read properties of undefined (reading 'call'); 7× TypeError: not a function; 6× TypeError: Cannot convert undefined or null to object |
| ShadowRealm | 64 | 0 | 64 | 61× ReferenceError: ShadowRealm is not defined; 2× Test262Error: This test must fail if ShadowRealm is not a function Expected SameValue(«"undefi; 1× Test262Error: ShadowRealm should be an own property |
| immutable-arraybuffer | 61 | 2 | 59 | 22× TypeError: not a function; 18× Test262Error: no arg factories match include immutable and exclude undefined; 7× Test262Error: Method must exist. Expected SameValue(«"undefined"», «"function"») to be true; 3× TypeError: Cannot read properties of undefined (reading 'get') |
| source-phase-imports | 143 | 86 | 57 | 48× SyntaxError: The only valid meta property for import is 'import.meta'; 8× Test262Error: Expected SameValue(«"undefined"», «"function"») to be true; 1× (no error line):  |
| source-phase-imports+source-phase-imports-module-source | 108 | 63 | 45 | 42× SyntaxError: The only valid meta property for import is 'import.meta'; 2× SyntaxError: Unexpected token 'x'; 1× SyntaxError: Unexpected token 'mod' |
| error-stack-accessor | 35 | 1 | 34 | 19× TypeError: Cannot read properties of undefined (reading 'set'); 13× TypeError: Cannot read properties of undefined (reading 'get'); 1× Test262Error: Error: Error.prototype has the accessor Expected SameValue(«undefined», «undefin; 1× Test262Error: Error.prototype.stack getter should be a function Expected SameValue(«"undefined |
| legacy-regexp | 26 | 0 | 26 | 7× TypeError: Cannot read properties of undefined (reading 'get'); 5× TypeError: Cannot read properties of undefined (reading 'set'); 2× Test262Error: Expected a TypeError to be thrown but no exception was thrown at all; 1× Test262Error: RegExp.$N getter throws for cross-realm receiver Expected a TypeError to be thro |
| decorators | 24 | 2 | 22 | 20× SyntaxError: Invalid or unexpected token; 2× SyntaxError: Unexpected token '$' |
| Iterator.prototype.join | 18 | 2 | 16 | 6× TypeError: not a function; 4× Test262Error: Expected a TestNError but got a TypeError; 2× TypeError: Cannot read properties of undefined (reading 'call'); 2× TypeError: Cannot convert undefined or null to object |
| Temporal | 8 | 0 | 8 | 2× TypeError: Cannot convert undefined or null to object; 2× Test262Error: Expected SameValue(«"undefined"», «"function"») to be true; 1× Test262Error: isConstructor invoked with a non-function value; 1× Test262Error: toTemporalInstant should be an own property |
| import-text | 6 | 0 | 6 | 2× SyntaxError: The requested module does not provide an export named 'default'; 1× (no error line): ; 1× SyntaxError: Unexpected token '{'; 1× SyntaxError: Unexpected token 'string' |
| immutable-arraybuffer+import-bytes | 5 | 0 | 5 | 2× SyntaxError: The requested module does not provide an export named 'default'; 1× SyntaxError: Unexpected token ':'; 1× SyntaxError: Invalid or unexpected token; 1× SyntaxError: Unexpected token 'World' |
| nonextensible-applies-to-private | 2 | 0 | 2 | 2× Test262Error: Expected a TypeError to be thrown but no exception was thrown at all |
| import-defer+nonextensible-applies-to-private | 1 | 0 | 1 | 1× SyntaxError: Unexpected token '*' |

## 8. Node 22 comparison

Node v22.23.3 ran through `t262/hosts/node-host.mjs` with the main runner (watchdog, `--jobs 4`, 700 MB RSS cap) over the extra set **without** `built-ins/Temporal`, `intl402/Temporal` and `staging/Temporal` (node 22 has no Temporal): 3,675 files / 6,998 scenarios. In those same 3,675 files, node passes 2,677 and we pass 1,709.

- **Intl:** node (full ICU) passes **1,052 / 1,353** non-Temporal intl402 files. That is what an ICU-backed Intl reaches; node has no DurationFormat (0 / 110) and fails DateTimeFormat 77, Locale 88, PluralRules 10. This is consistent with §4: our heuristic puts about 1,076 files within reach without ICU (pass + plumbing + BCP 47 + en-data), but only by writing the BCP 47 and en locale data ourselves.
- **Proposals:** node 22 is level with us on every proposal tag (it passes 1 more source-phase file and 1 legacy-regexp file), so node is no reference here. Every proposal failure is engine work.
- **staging:** node passes 22 of our 42 non-proposal staging failures, which confirms them as reachable. Node also fails the runner artefacts.
  - Node passes: `sm/Proxy/revoke-as-side-effect`, `sm/String/replace-math`, `sm/destructuring/bug1396261`, `sm/extensions/recursion`, `sm/Date/{non-iso,two-digit-years}`, all 4 Annex B edge cases (so they are engine gaps, not SM-only semantics), 10 of the 12 `function.caller` / `f.arguments` tests (`function-caller-restrictions` fails in node too), and `sm/statements/regress-642975` (RegExp.lastMatch).
  - Node fails too:
    - `sm/strict/*` (13; runner artefact confirmed).
    - `module-export-name-star` (self-import).
    - Both IsHTMLDDA tests (node-host has no `IsHTMLDDA`).
    - The 3 length-tracking-TA integrity tests (node 22 predates the spec change).
    - `sm/regress/regress-610026`: node is also killed by the 700 MB watchdog. The test is memory-hungry everywhere, so for us the fix is "throw or survive instead of aborting".
    - The proposal ones (decorators, source-phase, Temporal, PrivateName).
  - We pass, node fails: 83 files, mostly `staging/explicit-resource-management` (48, which node 22 lacks), `staging/sm` 29 and `staging/built-ins` 4.

| Cluster | Files | We pass | Node passes | Node passes, we fail | We pass, node fails |
| --- | ---: | ---: | ---: | ---: | ---: |
| intl402/NumberFormat | 251 | 0 | 249 | 249 | 0 |
| intl402/DateTimeFormat | 245 | 0 | 168 | 168 | 0 |
| intl402/Locale | 190 | 0 | 102 | 102 | 0 |
| intl402/ListFormat | 81 | 0 | 81 | 81 | 0 |
| intl402/RelativeTimeFormat | 80 | 0 | 79 | 79 | 0 |
| intl402/Segmenter | 79 | 0 | 79 | 79 | 0 |
| intl402/Collator | 65 | 0 | 62 | 62 | 0 |
| intl402/Intl | 66 | 0 | 61 | 61 | 0 |
| intl402/DisplayNames | 57 | 0 | 57 | 57 | 0 |
| intl402/PluralRules | 53 | 0 | 43 | 43 | 0 |
| staging/sm | 1406 | 1364 | 1357 | 22 | 29 |
| intl402/(top-level) | 22 | 0 | 20 | 20 | 0 |
| intl402/String | 19 | 7 | 17 | 10 | 0 |
| intl402/BigInt | 11 | 5 | 11 | 6 | 0 |
| intl402/Date | 12 | 8 | 12 | 4 | 0 |
| intl402/Number | 7 | 3 | 7 | 4 | 0 |
| proposal:source-phase-imports | 143 | 86 | 87 | 1 | 0 |
| proposal:legacy-regexp | 26 | 0 | 1 | 1 | 0 |
| intl402/Array | 2 | 1 | 2 | 1 | 0 |
| intl402/FallbackSymbol | 2 | 0 | 1 | 1 | 0 |
| proposal:import-defer | 251 | 94 | 94 | 0 | 0 |
| intl402/DurationFormat | 110 | 0 | 0 | 0 | 0 |
| proposal:source-phase-imports+source-phase-imports-module-source | 108 | 63 | 63 | 0 | 0 |
| proposal:await-dictionary | 89 | 2 | 2 | 0 | 0 |
| proposal:ShadowRealm | 64 | 0 | 0 | 0 | 0 |
| proposal:immutable-arraybuffer | 61 | 2 | 2 | 0 | 0 |
| staging/explicit-resource-management | 53 | 53 | 5 | 0 | 48 |
| proposal:error-stack-accessor | 35 | 1 | 0 | 0 | 1 |
| proposal:decorators | 24 | 2 | 2 | 0 | 0 |
| proposal:Iterator.prototype.join | 18 | 2 | 2 | 0 | 0 |
| proposal:Temporal | 8 | 0 | 0 | 0 | 0 |
| staging/built-ins | 8 | 5 | 1 | 0 | 4 |
| proposal:import-text | 6 | 0 | 0 | 0 | 0 |
| proposal:immutable-arraybuffer+import-bytes | 5 | 0 | 0 | 0 | 0 |
| staging/decorators | 3 | 1 | 1 | 0 | 0 |
| staging/set-methods | 3 | 3 | 3 | 0 | 0 |
| proposal:nonextensible-applies-to-private | 2 | 0 | 0 | 0 | 0 |
| staging/source-phase-imports | 2 | 0 | 0 | 0 | 0 |
| proposal:import-defer+nonextensible-applies-to-private | 1 | 0 | 0 | 0 | 0 |
| intl402/TypedArray | 1 | 1 | 1 | 0 | 0 |
| staging/set-is-subset-of-empty-index | 1 | 1 | 1 | 0 | 0 |
| staging/set-is-subset-on-set-like | 1 | 1 | 1 | 0 | 0 |
| staging/set-is-subset-table-receiver-cleared | 1 | 1 | 1 | 0 | 0 |
| staging/set-is-subset-table-transition | 1 | 1 | 1 | 0 | 0 |
| staging/top-level-await | 1 | 1 | 1 | 0 | 0 |
| staging/Uint8Array | 1 | 1 | 0 | 0 | 1 |
| **total** | **3675** | **1709** | **2677** | **1051** | **83** |
