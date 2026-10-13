// Summarizes a run's JSONL per directory: files passing (a file passes only if all its scenarios pass).
// node tools/runtime/summarize.mjs results/slice.jsonl [depth=3] [--fail]  (--fail lists failing files)
import fs from 'node:fs';
const [file = 'results/slice.jsonl', depthArg = '3', ...flags] = process.argv.slice(2);
const depth = Number(depthArg);
const files = new Map();
for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
  if (!line.startsWith('{"file"')) continue;
  const r = JSON.parse(line);
  const prev = files.get(r.file);
  if (!prev || r.status !== 'pass') files.set(r.file, r.status === 'pass' && (!prev || prev.status === 'pass') ? r : r);
  if (prev && prev.status !== 'pass') files.set(r.file, prev);
}
const dirs = new Map();
for (const [f, r] of files) {
  const d = f.split('/').slice(0, depth).join('/');
  const e = dirs.get(d) || { pass: 0, total: 0 };
  e.total++;
  if (r.status === 'pass') e.pass++;
  dirs.set(d, e);
}
let pass = 0, total = 0;
for (const [d, e] of [...dirs].sort((a, b) => (b[1].total - b[1].pass) - (a[1].total - a[1].pass))) {
  console.log(`${d} ${e.pass} / ${e.total}`);
  pass += e.pass;
  total += e.total;
}
console.log(`TOTAL ${pass} / ${total}`);
if (flags.includes('--fail')) {
  for (const [f, r] of files) if (r.status !== 'pass') console.log(`FAIL ${f} ${r.error_name || r.status}: ${(r.message || '').toString().slice(0, 160).replace(/\n/g, ' ')}`);
}
