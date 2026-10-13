// gate.mjs: the merge check. Lists every file that passes in the baseline and fails in the branch's run, with
// the failing scenarios' status and error text. Exit 1 if there is any (each must be fixed or explained), and
// exit 2 if the branch run doesn't cover a baseline file.
//
//   node tools/lead/gate.mjs results/<main sha>.json /tmp/core.jsonl [more.jsonl ...]
//
// The baseline is an aggregate (its passing_files); the branch side is raw JSONL from run.mjs, from any suite:
// only the baseline's suite is compared. Re-run a regression that is a timeout alone (run.mjs --filter <file>)
// before calling it flaky.
import fs from 'node:fs';

const [baselineFile, ...jsonl] = process.argv.slice(2);
if (!baselineFile || jsonl.length === 0) {
  console.error('usage: gate.mjs <baseline.json> <branch.jsonl>...');
  process.exit(64);
}
const baseline = JSON.parse(fs.readFileSync(baselineFile, 'utf8'));
const scenarios = new Map();
for (const file of jsonl) {
  for (const text of fs.readFileSync(file, 'utf8').split('\n')) {
    if (!text.trim()) continue;
    const line = JSON.parse(text);
    if (!scenarios.has(line.file)) scenarios.set(line.file, []);
    scenarios.get(line.file).push(line);
  }
}
const missing = [];
const regressions = [];
for (const file of baseline.passing_files) {
  const lines = scenarios.get(file);
  if (!lines) {
    missing.push(file);
    continue;
  }
  const failing = lines.filter((l) => l.status !== 'pass');
  if (failing.length) regressions.push({ file, failing });
}
const passing = [...scenarios].filter(([, lines]) => lines.every((l) => l.status === 'pass')).length;
console.log(`baseline ${baseline.label} (${baseline.suite}): ${baseline.passing_files.length} passing files; ` +
  `branch run: ${passing} passing of ${scenarios.size} files; regressions: ${regressions.length}; not run: ${missing.length}`);
for (const { file, failing } of regressions) {
  for (const l of failing) console.log(`REGRESSION ${file} [${l.mode}] ${l.status} ${l.ms}ms ${l.error_name ?? ''}: ${l.message ?? ''}`);
}
if (missing.length) console.log(`not run (first 10): ${missing.slice(0, 10).join(', ')}`);
process.exitCode = missing.length ? 2 : regressions.length ? 1 : 0;
