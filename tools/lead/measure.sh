#!/usr/bin/env bash
# measure.sh parse|full|suite [baseline.json]: run all of ES-core on this machine for the checked-out commit,
# aggregate into results/<sha>-parse.json (parse-only) or results/<sha>.json (full), print the milestones and
# the gap against node (tools/lead/nodegap.mjs). `suite` runs the whole full suite (--suite full) instead and
# aggregates it twice: results/<sha>-full.json (full suite) and results/<sha>.json (its ES-core subset).
# JSONL stays in results/<sha>/ (gitignored); commit only the aggregate. Run it in the background (ES-core
# ~30 min, the full suite longer at 4 jobs): a foreground tool call times out first.
#
# Guards, each from a real incident: one run per checkout at a time (two runs crash on each other's
# .t262-* temp files while listing scenarios); a clean tree; HEAD must not move while it runs (a pull and
# rebuild mid-run mixes binaries). Build and slice in a separate `git worktree` while a run is going.
set -euo pipefail
cd "$(dirname "$0")/../.."
mode=${1:-parse}
baseline=${2:-}
lock=results/.measure.lock
if ! mkdir "$lock" 2>/dev/null; then
  echo "measure.sh: another run holds $lock (remove it if that run is dead)" >&2
  exit 3
fi
trap 'rmdir "$lock"' EXIT
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "measure.sh: the working tree has changes; commit or stash them first" >&2
  exit 3
fi
sha=$(git rev-parse --short=9 HEAD)
make -s -j4 engine test262 >/dev/null
mkdir -p "results/$sha"
suite=es-core
if [ "$mode" = parse ]; then
  jsonl="results/$sha/parse.jsonl"; out="results/$sha-parse.json"; flag=--parse-only
elif [ "$mode" = suite ]; then
  jsonl="results/$sha/suite.jsonl"; out="results/$sha.json"; flag=; suite=full
else
  jsonl="results/$sha/full.jsonl"; out="results/$sha.json"; flag=
fi
node t262/runner/run.mjs --engine ./engine --suite "$suite" $flag --jobs 4 --out "$jsonl"
if [ "$(git rev-parse --short=9 HEAD)" != "$sha" ]; then
  echo "measure.sh: HEAD moved during the run; the results are not for one commit, discarding" >&2
  exit 4
fi
if [ "$suite" = full ]; then
  node t262/runner/aggregate.mjs --suite full --label "$sha" --out "results/$sha-full.json" "$jsonl" >/dev/null
fi
node t262/runner/aggregate.mjs --suite es-core --subset --label "$sha" ${baseline:+--baseline "$baseline"} --out "$out" "$jsonl" |
  node -e 'const s=JSON.parse(require("fs").readFileSync(0,"utf8")); console.log(JSON.stringify({complete:s.complete,files:s.files,buckets:s.buckets,diff:s.diff&&{newly_passing:s.diff.newly_passing,regressions:s.diff.regressions.length}}))'
if [ "$mode" = parse ]; then
  full=$(ls -t results/*.json 2>/dev/null | grep -v -e '-parse.json' -e '-full.json' | head -1 || true)
  node t262/runner/milestones.mjs $full --parse-only "$out"
else
  node t262/runner/milestones.mjs "$out" ${PARSE_RESULT:+--parse-only "$PARSE_RESULT"} \
    $([ "$suite" = full ] && echo --full "results/$sha-full.json")
  node tools/lead/nodegap.mjs "$out"
fi
