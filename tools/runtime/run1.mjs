// Runs one test262 file the way t262/runner does (harness + includes, sloppy or strict) and prints the
// engine's output. node tools/runtime/run1.mjs test/built-ins/X/y.js [strict]
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
const [file, mode = 'sloppy'] = process.argv.slice(2);
const root = 't262/test262';
const source = fs.readFileSync(path.join(root, file), 'utf8');
const meta = (source.match(/\/\*---([\s\S]*?)---\*\//) || [, ''])[1];
const list = (key) => {
  const m = meta.match(new RegExp(`${key}:\\s*\\[([^\\]]*)\\]`)) || meta.match(new RegExp(`${key}:\\s*\\n((?:\\s+-.*\\n)+)`));
  if (!m) return [];
  return m[1].split(/[,\n]/).map((s) => s.replace(/^\s*-\s*/, '').trim()).filter(Boolean);
};
const flags = list('flags'), includes = list('includes');
let composed = mode === 'strict' ? '"use strict";\n' : '';
if (!flags.includes('raw')) {
  const h = (n) => fs.readFileSync(path.join(root, 'harness', n), 'utf8');
  composed += h('assert.js') + h('sta.js');
  if (flags.includes('async')) composed += h('doneprintHandle.js');
  for (const inc of includes) composed += h(inc);
}
composed += source;
const out = path.join(path.dirname(path.join(root, file)), `.run1-${process.pid}.${flags.includes('module') ? 'mjs' : 'js'}`);
fs.writeFileSync(out, composed);
const args = flags.includes('module') ? ['--module', out] : [out];
const r = spawnSync(process.env.ENGINE || './engine', args, { encoding: 'utf8', timeout: 20000 });
fs.rmSync(out, { force: true });
process.stdout.write((r.stdout || '') + (r.stderr || ''));
console.log(`[exit ${r.status}${r.signal ? ' ' + r.signal : ''}]`);
