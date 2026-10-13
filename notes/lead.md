# Lead handoff (snowman-js)

Updated at every merge and measurement. A new lead (or the supervisor directly) continues from this file,
STATUS.md and AGENTS.md alone. Last update: 2026-10-13 ~16:30 UTC, main 317949e + results.

## Numbers and baselines

- Latest measured main: **317949e37**, full **53,414 / 53,616 (99.62%)**, ES-core **43,196 / 43,305 (99.75%)**, M8.
  `node t262/runner/milestones.mjs results/317949e37.json --full results/317949e37-full.json`
- Gate baselines: `results/317949e37.json` (ES-core) and `results/317949e37-full.json` (full); JSONL on the
  results branch at `results/317949e37/suite.jsonl`. Nothing merged since.
- Measure: `TMPDIR=$HOME/tmp tools/lead/measure.sh suite results/<previous>.json` in the background with a monitor
  (~22 min at 4 jobs). Then `gate.mjs <prev>-full.json results/<sha>/suite.jsonl` and `clusters.mjs <sha>-full.json
  <prev>-full.json`. Commit `results/<sha>{,-full}.json` to main, push the JSONL to the results branch (worktree
  under $HOME), update STATUS "Latest result", and send the supervisor milestones.mjs verbatim.

## Merge queue

1. builtins: ws/calendars-ref addd9a2 (chinese/dangi reference years, 4 files), gated vs a1a2a5fc3; re-gate its JSONL
   against 317949e37 before merging.
2. builtins: $262.agent step 3 (ws/agent3 738da48 WIP: SharedBlock, real atomics); then step 4 (agents, waiters).

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

Every spawn since 12:20 fails with "ProvisionSandbox ... no hosts match the requirements" (12:20, 14:33, 14:47,
15:45 "decorators"). Next: 16:50 (one-shot cron reminder).
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
