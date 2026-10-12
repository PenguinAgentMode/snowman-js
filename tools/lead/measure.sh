#!/usr/bin/env bash
# measure.sh parse|full [baseline.json]: run all of ES-core on this machine for the checked-out commit,
# aggregate into results/<sha>-parse.json (parse-only) or results/<sha>.json (full), print the milestones.
# Shard JSONL stays in results/<sha>/ (gitignored *.jsonl); commit only the aggregate.
set -euo pipefail
cd "$(dirname "$0")/../.."
mode=${1:-parse}
baseline=${2:-}
sha=$(git rev-parse --short=9 HEAD)
make -s engine test262 >/dev/null
mkdir -p "results/$sha"
if [ "$mode" = parse ]; then
  jsonl="results/$sha/parse.jsonl"; out="results/$sha-parse.json"; flag=--parse-only
else
  jsonl="results/$sha/full.jsonl"; out="results/$sha.json"; flag=
fi
node t262/runner/run.mjs --engine ./engine $flag --jobs "$(nproc)" --out "$jsonl"
node t262/runner/aggregate.mjs --suite es-core --label "$sha" ${baseline:+--baseline "$baseline"} --out "$out" "$jsonl" |
  node -e 'const s=JSON.parse(require("fs").readFileSync(0,"utf8")); console.log(JSON.stringify({complete:s.complete,files:s.files,buckets:s.buckets,diff:s.diff&&{newly_passing:s.diff.newly_passing,regressions:s.diff.regressions.length}}))'
if [ "$mode" = parse ]; then
  full=$(ls -t results/*.json 2>/dev/null | grep -v -- '-parse.json' | head -1 || true)
  node t262/runner/milestones.mjs $full --parse-only "$out"
else
  node t262/runner/milestones.mjs "$out" ${PARSE_RESULT:+--parse-only "$PARSE_RESULT"}
fi
