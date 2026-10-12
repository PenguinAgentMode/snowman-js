// astdiff.mjs: compare `engine --dump-ast` with acorn (ecmaVersion "latest") on JS files. acorn is a
// dev-only oracle: npm install --prefix tools acorn.
//
//   node tools/parser/astdiff.mjs [--engine ./engine] [--module] [--quiet] <file or directory>...
// Prints the first differing path per mismatching file and a summary. Files acorn rejects are skipped.
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const require = createRequire(path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'package.json'));
const acorn = require('acorn');

const args = process.argv.slice(2);
let engine = './engine';
let module = false;
let quiet = false;
const inputs = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--engine') engine = args[++i];
  else if (args[i] === '--module') module = true;
  else if (args[i] === '--quiet') quiet = true;
  else inputs.push(args[i]);
}

function walk(p, out) {
  if (fs.statSync(p).isDirectory()) {
    for (const entry of fs.readdirSync(p).sort()) if (!entry.startsWith('.')) walk(path.join(p, entry), out);
  } else if (p.endsWith('.js') || p.endsWith('.mjs')) {
    out.push(p);
  }
}

// acorn's tree as plain JSON: BigInt values as their decimal string, RegExp values as {}.
function normalize(tree) {
  return JSON.parse(JSON.stringify(tree, (key, value) => {
    if (typeof value === 'bigint') return value.toString();
    if (value instanceof RegExp) return {};
    if (key === 'value' && value === undefined) return {};
    return value;
  }));
}

function firstDifference(a, b, where) {
  if (typeof a !== typeof b || (a === null) !== (b === null) || Array.isArray(a) !== Array.isArray(b))
    return `${where}: ${JSON.stringify(a)?.slice(0, 120)} vs ${JSON.stringify(b)?.slice(0, 120)}`;
  if (Array.isArray(a)) {
    if (a.length !== b.length) return `${where}.length: ${a.length} vs ${b.length}`;
    for (let i = 0; i < a.length; i++) {
      const d = firstDifference(a[i], b[i], `${where}[${i}]`);
      if (d) return d;
    }
    return null;
  }
  if (a && typeof a === 'object') {
    const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
    const label = a.type ? `${where}<${a.type}@${a.start}>` : where;
    for (const k of keys) {
      if (!(k in a) || !(k in b)) return `${label}.${k}: missing on ${k in a ? 'engine' : 'acorn'} side`;
      const d = firstDifference(a[k], b[k], `${label}.${k}`);
      if (d) return d;
    }
    return null;
  }
  if (typeof a === 'number' && Number.isNaN(a) && Number.isNaN(b)) return null;
  return a === b ? null : `${where}: ${JSON.stringify(a)} vs ${JSON.stringify(b)}`;
}

const files = [];
for (const input of inputs) walk(input, files);
let same = 0, differ = 0, skipped = 0, engineFailed = 0;
for (const file of files) {
  const source = fs.readFileSync(file, 'utf8');
  const isModule = module || file.endsWith('.mjs');
  let expected;
  try {
    expected = normalize(acorn.parse(source, { ecmaVersion: 'latest', sourceType: isModule ? 'module' : 'script', allowHashBang: true }));
  } catch {
    skipped++;
    continue;
  }
  let actual;
  try {
    const out = execFileSync(engine, ['--dump-ast', ...(isModule ? ['--module'] : []), file], { encoding: 'utf8', maxBuffer: 1 << 28 });
    actual = JSON.parse(out);
  } catch (e) {
    engineFailed++;
    if (!quiet) console.log(`${file}: engine failed: ${(e.stdout || e.message).toString().trim().slice(0, 200)}`);
    continue;
  }
  const d = firstDifference(actual, expected, 'Program');
  if (d) {
    differ++;
    if (!quiet) console.log(`${file}: ${d}`);
  } else {
    same++;
  }
}
console.log(JSON.stringify({ files: files.length, same, differ, engineFailed, skippedByAcorn: skipped }));
if (differ || engineFailed) process.exitCode = 1;
