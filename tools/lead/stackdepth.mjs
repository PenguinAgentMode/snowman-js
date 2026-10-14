// stackdepth.mjs: the C-stack guard for interpreter changes. Probe programs recurse until the RangeError and print
// the depth reached, run under the runner's limits (ulimit -v 1 GiB, the engine's own 480 MB main stack). The
// deepest tests (test/language/statements/for/tco-*-body.js, 100,000 nested calls) need more than 432 MB of that
// stack, so a change must not lower any probe's depth: compare against main's engine before a gate.
//
//   node tools/lead/stackdepth.mjs [--keep-mem-limit] <engine> [<baseline engine>]
//
// Prints the depth and peak RSS per probe (and the baseline's, and the depth change). Exit 1 if a probe ends
// without a RangeError (a crash), or reaches a smaller depth than on the baseline engine.
//
// Both engines run with SNOWMAN_MEM_MB=0 (no soft memory limit, runtime/heap.cpp): with the limit, a probe that
// holds heap per level (spread) ends in its RangeError("out of memory") near 640 MB instead of at the end of the
// stack, so the depth would measure the heap, and an engine with the limit would compare as a drop against one
// without it. --keep-mem-limit runs them with the caller's environment instead.
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const VM_KB = 1048576;

// Each probe counts its depth in `n` and recurses through `start()` until the stack runs out. Argument lists:
// args-8 is the most that is evaluated onto the C stack (expressions.cpp kStackArguments), args-9 and spread take
// the ValueVector path. A spread of elements retains iterator garbage per level (conservative stack scan) and
// runs out of memory before the stack, on main too, so the spread probe spreads an empty array.
const PROBES = {
  plain: 'function f() { n++; f(); }\nfunction start() { f(); }',
  method: 'var o = { m() { n++; this.m(); } };\nfunction start() { o.m(); }',
  'args-8': 'function f(a, b, c, d, e, g, h, i) { n++; f(a, b, c, d, e, g, h, i); }\n' +
    'function start() { f(1, 2, 3, 4, 5, 6, 7, 8); }',
  'args-9': 'function f(a, b, c, d, e, g, h, i, j) { n++; f(a, b, c, d, e, g, h, i, j); }\n' +
    'function start() { f(1, 2, 3, 4, 5, 6, 7, 8, 9); }',
  spread: 'var xs = [];\nfunction f(a, b, c) { n++; f(a, b, c, ...xs); }\nfunction start() { f(1, 2, 3); }',
  getter: 'var o = { get g() { n++; return this.g; } };\nfunction start() { return o.g; }',
  callback: 'function f() { n++; [1].map(f); }\nfunction start() { f(); }',
  construct: 'function F() { n++; new F(); }\nfunction start() { new F(); }',
};

function source(body) {
  return `"use strict";\nvar n = 0;\n${body}\ntry { start(); print("crash " + n); } catch (e) {\n` +
    `  print((e instanceof RangeError ? "depth " : "error " + e + " ") + n);\n}\n`;
}

const TIME = fs.existsSync('/usr/bin/time') ? '/usr/bin/time' : null;

// The depth a probe reaches (0 if it ends without a RangeError), and the engine's peak RSS in MB (if measurable).
function measure(engine, file, dir) {
  const rssFile = path.join(dir, 'rss');
  const command = TIME ? `exec ${TIME} -f %M -o "${rssFile}" "$0" "$1"` : 'exec "$0" "$1"';
  const run = spawnSync('bash', ['-c', `ulimit -v ${VM_KB}; ${command}`, engine, file], {
    encoding: 'utf8',
    env: ENV,
  });
  const kb = TIME && fs.existsSync(rssFile) ? Number(fs.readFileSync(rssFile, 'utf8').trim().split('\n').pop()) : NaN;
  const rss = Number.isFinite(kb) ? Math.round(kb / 1024) : null;
  const match = /^depth (\d+)$/m.exec(run.stdout);
  if (match) return { depth: Number(match[1]), rss };
  const what = (run.stdout.trim().split('\n').pop() || '') + (run.signal ? ` signal ${run.signal}` : '');
  return { depth: 0, rss, failure: what || `exit ${run.status}` };
}

const args = process.argv.slice(2);
const keepMemLimit = args.includes('--keep-mem-limit');
const [engine, baseline] = args.filter((arg) => arg !== '--keep-mem-limit');
if (!engine) {
  console.error('usage: node tools/lead/stackdepth.mjs [--keep-mem-limit] <engine> [<baseline engine>]');
  process.exit(2);
}
const ENV = keepMemLimit ? process.env : { ...process.env, SNOWMAN_MEM_MB: '0' };
const dir = fs.mkdtempSync(path.join(process.env.TMPDIR || path.join(os.homedir(), 'tmp'), 'stackdepth-'));
let failed = false;
try {
  for (const [name, body] of Object.entries(PROBES)) {
    const file = path.join(dir, `${name}.js`);
    fs.writeFileSync(file, source(body));
    const mine = measure(path.resolve(engine), file, dir);
    let line = `${name.padEnd(10)} ${String(mine.depth).padStart(8)} ${String(mine.rss ?? '?').padStart(4)} MB`;
    if (mine.failure) {
      failed = true;
      line += `  FAIL (${mine.failure})`;
    }
    if (baseline) {
      const base = measure(path.resolve(baseline), file, dir);
      const change = base.depth ? ((mine.depth / base.depth - 1) * 100).toFixed(1) : '?';
      line += `   baseline ${String(base.depth).padStart(8)} ${String(base.rss ?? '?').padStart(4)} MB  (${change}%)`;
      if (mine.depth < base.depth) {
        failed = true;
        line += '  DEPTH DROP';
      }
    }
    console.log(line);
  }
} finally {
  fs.rmSync(dir, { recursive: true, force: true });
}
process.exit(failed ? 1 : 0);
