// nodegap.mjs results/<sha>.json [baseline=results/node/es-core.json]: classify every failing file as a real
// gap ("node passes, we fail") or a data point ("node fails too"), per top-level directory.
import fs from 'node:fs';
import { listScenarios } from '../../t262/runner/scenarios.mjs';
const [file, nodeFile = 'results/node/es-core.json'] = process.argv.slice(2);
const ours = JSON.parse(fs.readFileSync(file, 'utf8'));
const node = JSON.parse(fs.readFileSync(nodeFile, 'utf8'));
const weOk = new Set(ours.passing_files);
const nodeOk = new Set(node.passing_files);
const all = new Set();
// The suite's own file list, so it works for an es-core run and for the ES-core subset of a full-suite run.
for (const s of listScenarios(new URL('../../t262/test262', import.meta.url).pathname, ours.suite ?? 'es-core')) all.add(s.file);
const gap = {}, both = {};
let gapN = 0, bothN = 0, onlyUs = 0;
for (const f of all) {
  if (weOk.has(f)) { if (!nodeOk.has(f)) onlyUs++; continue; }
  const dir = f.split('/').slice(0, 3).join('/');
  if (nodeOk.has(f)) { gap[dir] = (gap[dir] ?? 0) + 1; gapN++; } else { both[dir] = (both[dir] ?? 0) + 1; bothN++; }
}
console.log(JSON.stringify({ files: all.size, we_pass: weOk.size, node_pass: node.files.passed, gap_node_passes_we_fail: gapN, node_fails_too: bothN, we_pass_node_fails: onlyUs }));
const show = (title, m) => console.log(title, Object.entries(m).sort((a, b) => b[1] - a[1]).slice(0, 15).map(([d, n]) => `${n} ${d}`).join(' | '));
show('GAP:', gap);
show('NODE FAILS TOO:', both);
if (process.argv.includes('--list')) for (const f of all) if (!weOk.has(f) && nodeOk.has(f)) console.log('gap', f);
