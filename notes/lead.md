# Lead handoff (snowman-js)

Updated at every merge and measurement. A new lead (or the supervisor directly) continues from this file,
STATUS.md and AGENTS.md alone. Last update: 2026-10-13 ~16:30 UTC, main 317949e + results.

## Numbers and baselines

- Latest measured main: **19dc92105**, full **53,452 / 53,616 (99.69%)**, ES-core **43,196 / 43,305 (99.75%)**, M8.
  `node t262/runner/milestones.mjs results/19dc92105.json --full results/19dc92105-full.json`
- Gate baselines: `results/19dc92105{,-full}.json`; JSONL on the results branch at `results/19dc92105/suite.jsonl`.
- Monitor misses 17:21–19:07 UTC were a stack limit (GS session depth, 390262), since raised. Stranded agents: perf,
  decorators2, staging (turns over the 300 KB per-message limit); their pushed work is what's left.
- Measure: `TMPDIR=$HOME/tmp tools/lead/measure.sh suite results/<previous>.json` in the background with a monitor
  (~22 min at 4 jobs). Then `gate.mjs <prev>-full.json results/<sha>/suite.jsonl` and `clusters.mjs <sha>-full.json
  <prev>-full.json`. Commit `results/<sha>{,-full}.json` to main, push the JSONL to the results branch (worktree
  under $HOME), update STATUS "Latest result", and send the supervisor milestones.mjs verbatim.

## Merge queue

1. Lead's integration gate int/dec-re 38e96b9 = main 2bbe05d + decorators2 5eada31 + staging-regexp 41e448c27
   (worktree $HOME/wt/int; JSONL $HOME/logs/int1.jsonl). If clean vs 19dc92105, merge 38e96b9.
2. Merged: builtins step 4 17f576c (c47ec06: ES-core 43,305 / 43,305) and statics-lint bdd3485 (2853690).
3. perf e7f8922: the lead gates it next (stranded agent); then tell perf2 to start batch 2.
4. staging-caller bc68f1d7e: gate after perf (both touch interp/interpreter.h).
5. chinese/stability: ws/runner-strict (runner change) after its repeats.

How I merge: check overlap with `comm -12 <(git diff --name-only $base main|sort) <(git diff --name-only $base
<sha>|sort)` (AGENTS.md older-base rule); merge the exact sha; `make -j4` warning-free; push; list it under
"Merges awaiting a post-merge measurement" in STATUS.

## Workers

| Agent (entry) | State | Area | Branch |
| --- | --- | --- | --- |
| builtins | LIVE since ~07:10 | $262.agent step 4 (ws/agent4 17f576c, gating); then the statics lint (ws/statics-lint bdd3485) | ws/agent4; notes on ws/builtins5 |
| chinese → "stability" | LIVE (reopened 18:15) | 3 repeats of 19dc92105 → tools/lead/flaky.md; parse-only (M1); then the strict-prefix runner change (ws/runner-strict f29154e WIP) | ws/stability |
| intlfmt → "hardening" | LIVE (reopened 18:15) | ASan+UBSan full suite in 4 shards (~22:40 done) → tools/lead/sanitizers.md + gated fixes | ws/hardening |
| decorators → "perf2" | LIVE (entry reused 19:20) | perf's batch 2 (ws/perf 42ff096) after the lead merges batch 1 e7f8922 | ws/perf2; notes/perf2.md |
| perf, decorators2, staging | STRANDED (turn > 300 KB), machines stopped 19:17 | pushed work gated by the lead: ws/perf e7f8922, ws/decorators2 5eada31, ws/staging-regexp 41e448c27, ws/staging-caller bc68f1d7e | — |
| memcap, temporal2, survey, temporal, intl | machines stopped 16:33 | — | all commits on origin |
| calendars, calendars2, calendars3, stability, hardening | never started (no hosts); reusable entries | — | — |

The agent table is capped at 17 entries and closing doesn't free one: reuse an entry by reopening it with
agent_send (a new brief; it gets a machine if a slot is free). Supervisor rules: never reopen a stranded entry
(perf, staging, decorators2, temporal, temporal2, intl, memcap, survey: their threads hold only their prompts);
record entry → role here; at most 6 working at once.

Dead agents still list as IN_PROGRESS/IDLE and probably still hold machines; agent_close doesn't free them.

## Spawn retries

Capacity came back at 16:33 when the stack stopped the dead agents' machines. agent_close does NOT free a machine:
after closing chinese and intlfmt, new spawns (stability, hardening, 18:08) failed with "no hosts". So when a
worker finishes, keep it open and reuse it (agent_send reopens it with a fresh brief) instead of closing it.
Wanted if a slot frees: "hardening" (ASan+UBSan full suite in shards; brief in this file's history, 18:08).
The lead does stability (3 full-suite repeats + flaky list) and the parse-only run itself, in the background.

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
