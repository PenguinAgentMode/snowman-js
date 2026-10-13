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

1. Merged addd9a2 (reference years, +4); not yet measured (measure with the next batch).
2. builtins: $262.agent step 3 (ws/agent3 738da48 WIP: SharedBlock, real atomics); then step 4 (agents, waiters).

How I merge: check overlap with `comm -12 <(git diff --name-only $base main|sort) <(git diff --name-only $base
<sha>|sort)` (AGENTS.md older-base rule); merge the exact sha; `make -j4` warning-free; push; list it under
"Merges awaiting a post-merge measurement" in STATUS.

## Workers

| Agent | State | Area | Branch |
| --- | --- | --- | --- |
| builtins | LIVE since ~07:10 | $262.agent step 3 (ws/agent3 d995052, gating vs 317949e37), then step 4 | ws/agent3; notes/builtins.md + notes/agent-design.md on ws/builtins5 |
| chinese | LIVE 16:45 | astronomical Chinese/Dangi tables (9 intl402/Temporal files) | ws/chinese; notes/chinese.md |
| decorators2 | started 16:46 | decorators (22 + 2 staging), from ws/decorators fb2d287 | ws/decorators2; notes/decorators2.md |
| staging | started 16:45 | staging triage (34); fixes + tools/lead/staging-residue.md | ws/staging; notes/staging.md |
| intlfmt | LIVE 16:45 | non-Gregorian DTF output (~17 intl402), Ramadan (6), tz link (1) | ws/intlfmt; notes/intlfmt.md |
| memcap, temporal2, survey, temporal, intl | machines stopped 16:33 | — | all their commits are on origin (see STATUS) |
| calendars, calendars2, calendars3, decorators | never started ("no hosts") | — | — |

Dead agents still list as IN_PROGRESS/IDLE and probably still hold machines; agent_close doesn't free them.

## Spawn retries

Capacity is back since 16:33 (dead machines stopped). Working agents: lead + builtins + 4 = 6 (the limit).
Don't spawn more unless one closes.

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
