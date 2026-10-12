// milestones.mjs: which rungs of the ladder a results file reaches. Parse-only results (from
// run.mjs --parse-only) are checked for M1; full-run results for M2 onwards.
//
//   node runner/milestones.mjs results/<sha>.json [--parse-only results/<sha>-parse.json]
import fs from 'node:fs';

const percent = (group) => (group && group.files ? (100 * group.passed) / group.files : 0);

function rungs(full, parse) {
  const dir = (name) => full?.dirs?.[name];
  const esCore = full?.files?.percent ?? 0;
  return [
    { id: 'M1a', target: 'parse-only: ≥95% of positives parse, ≥50% of negative-parse tests rejected',
      reached: Boolean(parse) && percent(parse.buckets.positive) >= 95 && percent(parse.buckets.negative_parse) >= 50,
      measured: parse ? `${percent(parse.buckets.positive).toFixed(1)}% / ${percent(parse.buckets.negative_parse).toFixed(1)}%` : 'no parse-only run' },
    { id: 'M1b', target: 'parse-only: ≥99.5% / ≥95%',
      reached: Boolean(parse) && percent(parse.buckets.positive) >= 99.5 && percent(parse.buckets.negative_parse) >= 95,
      measured: parse ? `${percent(parse.buckets.positive).toFixed(1)}% / ${percent(parse.buckets.negative_parse).toFixed(1)}%` : 'no parse-only run' },
    { id: 'M2', target: 'test/harness ≥85 of 116', reached: (dir('test/harness')?.passed ?? 0) >= 85, measured: `${dir('test/harness')?.passed ?? 0}/${dir('test/harness')?.files ?? 0}` },
    { id: 'M3', target: 'no-feature language ≥80%, ES-core ≥20%', reached: percent(full?.buckets?.es5_language) >= 80 && esCore >= 20, measured: `${percent(full?.buckets?.es5_language).toFixed(1)}%, ES-core ${esCore}%` },
    { id: 'M4', target: 'ES-core ≥35%', reached: esCore >= 35, measured: `${esCore}%` },
    { id: 'M5', target: 'ES-core ≥55%', reached: esCore >= 55, measured: `${esCore}%` },
    { id: 'M6', target: 'ES-core ≥70%', reached: esCore >= 70, measured: `${esCore}%` },
    { id: 'M7', target: 'ES-core ≥85%', reached: esCore >= 85, measured: `${esCore}%` },
    { id: 'M8', target: 'ES-core ≥95%', reached: esCore >= 95, measured: `${esCore}%` },
  ];
}

const args = process.argv.slice(2);
const parseIndex = args.indexOf('--parse-only');
const parseFile = parseIndex >= 0 ? args.splice(parseIndex, 2)[1] : null;
const full = args[0] ? JSON.parse(fs.readFileSync(args[0], 'utf8')) : null;
const parse = parseFile ? JSON.parse(fs.readFileSync(parseFile, 'utf8')) : null;

if (full && !full.complete) console.log('WARNING: the full run is incomplete; its numbers are not a milestone');
let highest = 'none';
for (const rung of rungs(full, parse)) {
  console.log(`${rung.reached ? 'REACHED' : 'not yet'}  ${rung.id}  ${rung.target}  (${rung.measured})`);
  if (rung.reached) highest = rung.id;
}
console.log(`highest: ${highest}`);
