# Lead handoff (snowman-js)

Updated at every merge and measurement. A new lead (or the supervisor directly) continues from this file,
STATUS.md and AGENTS.md alone. Last update: 2026-10-13 21:45 UTC, main 2a7dc0e (f.caller merged as 9c15385).

## Numbers and baselines

- Latest measured main: **5f3218f30**, full **53,587 / 53,616 (99.95%)**, ES-core **43,305 / 43,305 (100%)**, M8.
  `node t262/runner/milestones.mjs results/5f3218f30.json --full results/5f3218f30-full.json`
- Gate baselines: `results/5f3218f30{,-full}.json`; JSONL on the results branch at `results/5f3218f30/suite.jsonl`.
- Measure: `TMPDIR=$HOME/tmp tools/lead/measure.sh suite results/<previous>.json` in the background with a monitor
  (~22 min at 4 jobs). Then `gate.mjs <prev>-full.json results/<sha>/suite.jsonl` and `clusters.mjs <sha>-full.json
  <prev>-full.json`. Commit `results/<sha>{,-full}.json` to main, push the JSONL to the results branch (worktree
  under $HOME), update STATUS "Latest result", and send the supervisor milestones.mjs verbatim.

## Merge queue (21:45 UTC)

All milestones are REACHED: parse-only 100% (results/19dc92105-parse.json), ES-core 43,305 / 43,305, full 53,587
measured at 5f3218f30. f.caller is merged (9c15385, gated +12), so expect ~53,599.
1. int/perf c1214e2 = main 2a7dc0e + perf2 757a0f6 (perf batch 1 + frame fix), integrated by the lead because it
   overlapped f.caller (function.cpp includes). 4 tco tests pass at 453–468 MB; stackdepth vs main: all deeper. Full
   gate running in $HOME/wt/int4 → $HOME/logs/int4.{jsonl,done}; gate vs results/5f3218f30{,-full}.json. Merge
   c1214e2 if clean AND perf2's own gate of 757a0f6 is clean (perf2 sends GC-stress + summed ms).
2. builtins: ws/heaplimit 8191d53 (soft RSS limit SNOWMAN_MEM_MB=640 → RangeError "out of memory" after relief).
   Stackdepth + tco numbers ~21:55, full gate ~22:35. The spread probe (696 MB on int/perf) must end in RangeError.
3. stability (entry "chinese"): ws/runner-strict, the approved runner change for the 13 sm/strict files. New-runner
   full run ~21:51, parse-only old/new ~22:20/22:50. It's a RUNNER gain: record old vs new per file in STATUS.
4. perf2 (entry "decorators"): batch 2 ws/perf2b c1efddd, to rebase after c1214e2 lands.
5. hardening (entry "intlfmt"): ASan 0 reports over all 102,994 scenarios; UBSan-only sweep 21:40 → ~22:45; then a
   probe proving the sanitizers fire. tools/lead/sanitizers.md on ws/hardening, to merge at the end.
After 1+2 land: measure main once (expect ~53,599 + nothing lost).
Wake-ups: notices can be held after a notice-started turn, so ping workers by agent_send at their ETAs.

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
