// oracle.mjs: differential test of snowman::regexp::validate against node's RegExp constructor.
//   node tools/regexp/oracle.mjs [--cases cases.jsonl] [--fuzz N] [--grammar N] [--seed S] [--properties] [--show K]
// --cases: extract.mjs output (test262's literals); --fuzz: token soup; --grammar: generated patterns, half of them
// mutated; --properties: every \p{...} name and alias in every position, with near misses. With no --cases, the
// default is --fuzz 100000 --grammar 100000.
// Builds build/validate_driver if needed. node 22 ships two ES2025 RegExp features behind flags
// (modifiers, duplicate named groups): this script re-runs itself with them.
import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { runDriver } from './driver.mjs';
import { KNOWN_SYNTAX as KNOWN, fuzz, grammarFuzz, propertyCases } from './gen.mjs';

const NODE_FLAGS = ['--js-regexp-modifiers', '--js-regexp-duplicate-named-groups'];
const supportsModifiers = (() => {
  try {
    new RegExp('(?i:a)');
    return true;
  } catch {
    return false;
  }
})();
if (!supportsModifiers) {
  const r = spawnSync(process.execPath, [...NODE_FLAGS, ...process.argv.slice(1)], { stdio: 'inherit' });
  process.exit(r.status ?? 1);
}

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(`--${name}`);
  return i < 0 ? dflt : args[i + 1];
};



const nodeVerdict = (pattern, flags) => {
  try {
    new RegExp(pattern, flags);
    return true;
  } catch (e) {
    if (!(e instanceof SyntaxError)) throw e;
    return false;
  }
};

// ---- main ----
let cases = [];
const caseFile = opt('cases');
if (caseFile) cases = cases.concat(readFileSync(caseFile, 'utf8').trim().split('\n').map((l) => JSON.parse(l)));
const nFuzz = Number(opt('fuzz', caseFile ? 0 : 100000));
if (nFuzz) cases = cases.concat(fuzz(nFuzz, Number(opt('seed', 1))));
const nGrammar = Number(opt('grammar', caseFile ? 0 : 100000));
if (nGrammar) cases = cases.concat(grammarFuzz(nGrammar, Number(opt('seed', 1)) + 7));
if (args.includes('--properties')) cases = cases.concat(propertyCases());
// unique (pattern, flags)
const seen = new Map();
for (const c of cases) {
  const key = `${c.flags}/${c.pattern}`;
  if (!seen.has(key)) seen.set(key, c);
}
cases = [...seen.values()];
const ours = runDriver(cases);
let agree = 0;
const mismatches = [];
const known = new Map();
for (let i = 0; i < cases.length; i++) {
  const c = cases[i];
  const mine = ours[i].startsWith('1');
  const theirs = nodeVerdict(c.pattern, c.flags);
  if (mine === theirs) {
    agree++;
    continue;
  }
  const k = KNOWN.find((x) => x.test(c.pattern, c.flags, ours[i]));
  if (k) {
    known.set(k.why, (known.get(k.why) ?? 0) + 1);
    continue;
  }
  mismatches.push({ ...c, ours: ours[i], node: theirs });
}
const valid = cases.filter((c) => nodeVerdict(c.pattern, c.flags)).length;
console.log(`${cases.length} patterns (${valid} valid in node): agree ${agree}, known spec/V8 differences ${[...known.values()].reduce((a, b) => a + b, 0)}, mismatches ${mismatches.length}`);
for (const [why, n] of known) console.log(`  known: ${why}: ${n}`);
const show = Number(opt('show', 40));
for (const m of mismatches.slice(0, show)) {
  console.log(`  /${JSON.stringify(m.pattern).slice(1, -1)}/${m.flags}  node:${m.node ? 'valid' : 'error'}  ours:${m.ours}${m.file ? `  (${path.relative(root, m.file)})` : ''}`);
}
process.exitCode = mismatches.length ? 1 : 0;
