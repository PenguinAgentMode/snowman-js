# staging (agent "staging") handoff

Task: triage/fix the 34 failing test/staging files at main 317949e37 (lead's list). Baselines:
results/317949e37.json (ES-core), results/317949e37-full.json (full). Deliverable also: tools/lead/staging-residue.md.

## Branches (each its own gated merge, all based on origin/main 53d4d86)

| Branch | sha | What | State |
| --- | --- | --- | --- |
| ws/staging-date | 8d8a20932 | Date.parse SpiderMonkey legacy heuristics (date.cpp only): sm/Date/non-iso.js, two-digit-years.js | full gate running/done |
| ws/staging-regexp | 41e448c27 | regexp parser reads the pattern in place (no 4 B/char copy): sm/RegExp/unicode-{class-,}braced.js need ~921 MB of address space instead of ~1046 (cap 1024) | slices pass; gate next |
| ws/staging-caller | WIP | legacy f.caller / f.arguments (lead approved; 2 interface commits: InterpContext.isEval/.args, realm intrinsics %LegacyArgumentsGetter%/%LegacyCallerGetter%) | 12 staging files pass alone; slices + gate next |

## Gotchas

- Gate in a separate worktree ($HOME/gate) with its own copy of t262/test262 and a copied engine: two runner
  processes on one test262 tree crash each other (temp files next to the tests).
- The tool's foreground limit is ~10 min: slices of built-ins/RegExp take ~10 min under gate load; background them.
- Memory need: bisect `ulimit -v` ($HOME/vmneed.sh); gVisor /proc has no VmPeak/VmHWM; /usr/bin/time -v gives max RSS.

## Residue (to be written to tools/lead/staging-residue.md)

- 13 sm/strict/*: runner artefact (strict prefix on the harness).
- 2 decorators/*-auto-accessor.js: proposal (decorators2 owns class.cpp).
- 2 B.3.2 arguments block-function tests: contradict ES-core annexB block-decl-func-skip-arguments.js.
- sm/regress/regress-610026.js: AST/parser Scope memory for 2^21 blocks in eval (parser/, big change).
