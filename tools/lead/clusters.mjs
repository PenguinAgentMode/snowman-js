// clusters.mjs results/<sha>-full.json [results/<old>-full.json]: the full suite's passing files split into the
// clusters PLAN.md tracks (ES-core, Temporal, intl402, staging, proposals in ES-core's directories), with the
// change against an older full-suite result when given. Markdown, for STATUS.md.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { listScenarios } from '../../t262/runner/scenarios.mjs';

const test262 = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../t262/test262');
const [file, oldFile] = process.argv.slice(2);
const load = (f) => JSON.parse(fs.readFileSync(f, 'utf8'));
const now = load(file);
const old = oldFile ? load(oldFile) : null;
if (now.suite !== 'full') console.error(`warning: ${file} is suite ${now.suite}, not full`);

const core = new Set(listScenarios(test262, 'es-core').map((s) => s.file));
const all = [...new Set(listScenarios(test262, 'full').map((s) => s.file))];
function cluster(f) {
  if (core.has(f)) return 'ES-core';
  if (f.startsWith('test/built-ins/Temporal/')) return 'built-ins/Temporal';
  if (f.startsWith('test/intl402/Temporal/')) return 'intl402/Temporal';
  if (f.startsWith('test/intl402/')) return 'intl402 (other)';
  if (f.startsWith('test/staging/')) return 'staging';
  return 'proposals (in ES-core dirs)';
}
const order = ['ES-core', 'built-ins/Temporal', 'intl402/Temporal', 'intl402 (other)', 'staging', 'proposals (in ES-core dirs)'];
const rows = new Map(order.map((c) => [c, { files: 0, now: 0, old: 0 }]));
const passNow = new Set(now.passing_files);
const passOld = new Set(old?.passing_files ?? []);
for (const f of all) {
  const r = rows.get(cluster(f));
  r.files++;
  if (passNow.has(f)) r.now++;
  if (passOld.has(f)) r.old++;
}
const total = { files: 0, now: 0, old: 0 };
console.log(`| Cluster | Passing (${now.label}) | Files | %${old ? ` | vs ${old.label}` : ''} |`);
console.log(`| --- | ---: | ---: | ---: |${old ? ' ---: |' : ''}`);
for (const [c, r] of rows) {
  for (const k of ['files', 'now', 'old']) total[k] += r[k];
  console.log(`| ${c} | ${r.now.toLocaleString('en-US')} | ${r.files.toLocaleString('en-US')} | ${((100 * r.now) / r.files).toFixed(2)}${old ? ` | ${r.now - r.old >= 0 ? '+' : ''}${r.now - r.old}` : ''} |`);
}
console.log(`| **full suite** | **${total.now.toLocaleString('en-US')}** | **${total.files.toLocaleString('en-US')}** | **${((100 * total.now) / total.files).toFixed(2)}** |${old ? ` ${total.now - total.old >= 0 ? '+' : ''}${total.now - total.old} |` : ''}`);
