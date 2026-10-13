# Lead handoff (snowman-js)

Updated at every merge and measurement. A new lead (or the supervisor directly) continues from this file,
STATUS.md and AGENTS.md alone. Last update: 2026-10-13 ~15:10 UTC, main 9df2b3f.

## Numbers and baselines

- Latest measured main: **29a14dfdd**, full **51,639 / 53,616 (96.31%)**, ES-core **43,194 / 43,305 (99.74%)**, M8.
  `node t262/runner/milestones.mjs results/29a14dfdd.json --full results/29a14dfdd-full.json`
- Gate baselines: `results/29a14dfdd.json` (ES-core) and `results/29a14dfdd-full.json` (full); JSONL on the
  results branch at `results/29a14dfdd/suite.jsonl`.
- Merged since then, not measured: 7f86ee9 (builtins step 2, thread_local), 5f1393b (canblock-r 7205b4d, runner +
  CLI). Their gates: ES-core 0 / full 0 regressions (7205b4d: ES-core 43,196, full 51,641).
- Measure: `TMPDIR=$HOME/tmp tools/lead/measure.sh suite results/<previous>.json` in the background with a monitor
  (~22 min at 4 jobs). Then `gate.mjs <prev>-full.json results/<sha>/suite.jsonl` and `clusters.mjs <sha>-full.json
  <prev>-full.json`. Commit `results/<sha>{,-full}.json` to main, push the JSONL to the results branch (worktree
  under $HOME), update STATUS "Latest result", and send the supervisor milestones.mjs verbatim.

## Merge queue

1. builtins: ws/calendars-b **10979b4** (temporal2's framework 67c5b59 + gregory + chinese/dangi/islamic-umalqura).
   Slices vs 29a14dfdd: +589 files, 0 regressions; built-ins/Temporal 4,605. Its full gate is running on builtins'
   machine. Merge exactly the gated sha. Then measure main.
2. builtins next: calendar group 2 (buddhist/roc/japanese, ws/calendars-g2), then group 3 (arithmetic).

How I merge: check overlap with `comm -12 <(git diff --name-only $base main|sort) <(git diff --name-only $base
<sha>|sort)` (AGENTS.md older-base rule); merge the exact sha; `make -j4` warning-free; push; list it under
"Merges awaiting a post-merge measurement" in STATUS.

## Workers

| Agent | State | Area | Branch / last push |
| --- | --- | --- | --- |
| builtins | LIVE (the only one) | Temporal calendars (framework, all groups); $262.agent step 3 parked | ws/calendars-b 10979b4, ws/calendars-g2; notes/builtins.md + notes/agent-design.md on ws/builtins5 |
| memcap | dead 14:19 (hung in a call) | — | ws/canblock ff86a5f (merged via 7205b4d), ws/measure (notes/memcap.md, es-core-residue.md) |
| temporal2 | dead 13:47 (hung) | — | ws/calendars 67c5b59 (taken over), ws/decorators fb2d287 (decorators parser WIP, parked), notes/temporal2.md |
| survey | lost 12:15 (GS restart) | — | ws/survey-intl 5aba3f349 (merged) |
| temporal | dead ~09:00 (hung) | — | ws/temporal 7526397 (merged) |
| intl | dead (context > 1M tokens) | — | ws/intl b48382d (merged); ws/intl-dev was taken over |
| calendars, calendars2, calendars3 | never started ("no hosts") | — | — |

Dead agents still list as IN_PROGRESS/IDLE and probably still hold machines; agent_close doesn't free them.

## Spawn retries

Every spawn since 12:20 fails with "ProvisionSandbox ... no hosts match the requirements" (12:20, 14:33, 14:47).
Supervisor: retry once an hour (next 15:50; a one-shot cron reminder is set), or at once if told machines were
freed. A failed spawn: agent_close it and tell the supervisor.

## Open work (beyond the calendars)

- intl402/Temporal: 1,775 failing at 29a14dfdd, mostly calendars (builtins).
- decorators: 22 proposals + 2 staging; parser WIP on ws/decorators fb2d287 (temporal2). Unowned.
- $262.agent step 3 (112 ES-core files): design in notes/agent-design.md (builtins); parked.
- intl402 non-Temporal 35: mostly calendars; ~10 non-Gregorian DTF output (relatedYear/yearName, islamic eras).
- staging 34: runner artefacts (13 sm/strict), non-standard host features, memory hogs. No tiny fixes.

## Rules from today's incidents (AGENTS.md has them all)

- /tmp is a tmpfs in the machine's 4 GB: TMPDIR=$HOME/tmp; run.mjs has a machine guard (pause 78%, kill at 86%).
- No foreground call over ~10 min: background + completion monitor + end the turn.
- Push WIP every 20 min; notes/<agent>.md; status every 30 min; 45 min silent = presumed dead.
- Merge only the exact gated sha; ES-core regressions revert; full-only ones need a waiver with an owner.
- Keep output small (an agent died of context overflow).
- Sibling agents can't message each other: the lead relays.

## Waiting on the user / supervisor

- Freeing the dead agents' machines (intl 7300, temporal 7296, survey 7292, temporal2 7304, memcap 7288).
