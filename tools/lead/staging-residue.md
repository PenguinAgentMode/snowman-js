# test/staging: the files left failing (agent "staging")

Start: main 317949e37, staging 1,449 / 1,483 files, 34 failing (`results/317949e37/suite.jsonl`).

## Fixed (each its own gated merge)

| Files | Fix | Branch / sha |
| ---: | --- | --- |
| 2 | `sm/Date/non-iso.js`, `sm/Date/two-digit-years.js`: Date.parse fallback after SpiderMonkey's legacy grammar (loose dashed ISO, m/d/yy two-digit years, month-name orders) | ws/staging-date 8d8a20932, merged (main b84715f) |
| 2 | `sm/RegExp/unicode-braced.js`, `unicode-class-braced.js`: the regexp parser reads the pattern in place (no 4 B/char copy per parse); ~1,046 → ~921 MB of address space, under the runner's 1,024 MB cap | ws/staging-regexp 41e448c27, gating |
| 12 | legacy `f.caller` / `f.arguments` (17.1 extension, non-strict functions only; strict, generator, async and built-in callers read as null; eval frames skipped): `sm/Function/function-caller-restrictions`, `sm/extensions/{function-properties,censor-strict-caller,arguments-property-access-in-function,function-caller-skips-eval-frames}`, `sm/regress/regress-{584355,586482-1,586482-2,586482-3,586482-4,577648-1,577648-2}` | ws/staging-caller, gating |

None of the regress-* oddities had a separate engine bug: all 7 were `caller`.

## Left (18 files): known gaps

| Files | Cause | Kind | Detail |
| ---: | --- | --- | --- |
| 13 | `sm/strict/{8.7.2,8.12.5,8.12.7,8.12.7-2,10.4.2,10.6,12.10.1,15.3.5.1,15.3.5.2,15.5.5.1,15.5.5.2,15.10.7,eval-variable-environment}.js` | runner artefact | The runner's strict scenario prepends `"use strict"` to harness + test, so `non262-strict-shell.js`'s lenient-mode helpers become strict too. Each fails only in the strict scenario; node fails the same 13, strict scenario only (checked with t262/hosts/node-host.mjs). Not an engine change; t262/ left alone. |
| 2 | `decorators/{private,public}-auto-accessor.js` | proposal | `accessor` class fields (decorators proposal): parser + class.cpp, owned by decorators2. |
| 2 | `sm/lexical-environment/block-scoped-functions-annex-b-arguments.js`, `sm/regress/regress-602621.js` | contradicts ES-core | They expect a block `function arguments(){}` to replace the function's `arguments` (old B.3.2.1). The current spec skips the name `arguments`, and ES-core's `annexB/language/function-code/block-decl-func-skip-arguments.js` (which we pass) asserts the opposite. |
| 1 | `sm/regress/regress-610026.js` | too big (memory) | evals 2^21 then ~2^22 empty `{}` blocks: ~400 B per block in the parser (a `Scope` per block kept by `scopePool_` until the parse ends, plus the node), and an eval's AST arena is never freed, so ~2.4 GB. Fix: reuse/free parser Scopes at exitScope and free eval ASTs (src/parser/, not in this workstream). |

After both pending merges: staging 1,449 + 16 = 1,465 / 1,483 (98.8%), the 18 above left.
