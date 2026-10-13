// run.mjs: run one shard of a test262 suite against an engine, one process per scenario, and write
// a JSONL line per scenario. Tests run in place, so module specifiers resolve as INTERPRETING.md says: a script
// test is composed (harness + test + RAN) into a temp file next to it; a module test runs unmodified at its
// own path (a test that imports itself gets the same module record), with the harness as a --prelude Script
// and the RAN line as an --epilogue, both temp files in os.tmpdir() (INTERFACES.md section 1).
//
//   node runner/run.mjs --engine "<command>" [--suite es-core|full] [--shard k/N] [--jobs J (4)]
//                       [--parse-only] [--filter <path prefix>] [--out results/shard.jsonl]
//                       [--mem-mb M (env T262_MEM_MB, 700)] [--vm-mb V (env T262_VM_MB, 1024; 0: none)]
//
// Memory: a watchdog polls each engine's VmRSS and SIGKILLs one above --mem-mb; the scenario fails with
// error_name MemoryLimit. A runaway test then fails alone instead of taking the machine down (4 jobs x the cap
// has to fit the machine). Each engine also runs under `ulimit -v` --vm-mb, for growth faster than the poll
// (the engine's 480 MB main stack is reserved address space, most of it never touched).
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { listScenarios, metadataOf, shardOf } from './scenarios.mjs';

const KIT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const TIMEOUT_MS = 10_000;
const MEM_POLL_MS = 20;
const DONOTEVALUATE = 'Test262: This statement should not be evaluated.';
// RAN is printed after a test's body, so an engine that runs nothing and exits 0 can't pass it.
const RAN = 'Test262:Ran';
// The engine's line before an error thrown by a prelude or epilogue: the harness's error, never the test's.
const HOST_SCRIPT_ERROR = 'Test262:HostScriptError';

function parseArgs(argv) {
  const args = {
    suite: 'es-core',
    shard: '0/1',
    jobs: 4,
    parseOnly: false,
    test262: path.join(KIT, 'test262'),
    memMb: process.env.T262_MEM_MB || 700,
    vmMb: process.env.T262_VM_MB || 1024,
  };
  for (let i = 0; i < argv.length; i++) {
    const key = argv[i];
    if (key === '--parse-only') {
      args.parseOnly = true;
      continue;
    }
    args[key.replace(/^--/, '').replace(/-(\w)/g, (_, c) => c.toUpperCase())] = argv[++i];
  }
  if (!args.engine) throw new Error('--engine "<command>" is required');
  return args;
}

const harnessCache = new Map();
function harness(test262, name) {
  if (!harnessCache.has(name)) harnessCache.set(name, fs.readFileSync(path.join(test262, 'harness', name), 'utf8'));
  return harnessCache.get(name);
}

// The harness a test runs after: assert.js, sta.js, doneprintHandle.js for async tests, then its includes.
function harnessOf(test262, metadata) {
  const prelude = [harness(test262, 'assert.js'), harness(test262, 'sta.js')];
  if (metadata.flags.includes('async')) prelude.push(harness(test262, 'doneprintHandle.js'));
  for (const include of metadata.includes) prelude.push(harness(test262, include));
  return prelude.join('\n') + '\n';
}

// compose is the source a script scenario runs: the harness, the test's includes, then the test.
function compose(test262, source, metadata, mode) {
  const strict = mode === 'strict' ? '"use strict";\n' : '';
  if (metadata.flags.includes('raw')) return strict + source;
  return strict + harnessOf(test262, metadata) + source + `\n;print(${JSON.stringify(RAN)});\n`;
}

// verdict decides a scenario from the engine's exit code and output.
function verdict({ code, output, timedOut, memoryExceeded }, metadata, parseOnly) {
  if (timedOut) return 'timeout';
  if (memoryExceeded) return 'fail';
  if (output.includes(HOST_SCRIPT_ERROR)) return 'fail';
  const negative = metadata.negative;
  const expectsParseError = negative && (negative.phase === 'parse' || negative.phase === 'resolution');

  if (parseOnly) {
    if (expectsParseError) return code !== 0 && output.includes(negative.type) ? 'pass' : 'fail';
    return code === 0 ? 'pass' : 'fail';
  }
  if (negative) {
    const threwExpected = code !== 0 && output.includes(negative.type) && !output.includes(DONOTEVALUATE);
    return threwExpected ? 'pass' : 'fail';
  }
  if (code !== 0) return 'fail';
  const ranToTheEnd = metadata.flags.includes('raw') || output.includes(RAN);
  if (!ranToTheEnd) return 'fail';
  if (!metadata.flags.includes('async')) return 'pass';
  const completed = output.includes('Test262:AsyncTestComplete') && !output.includes('Test262:AsyncTestFailure');
  return completed ? 'pass' : 'fail';
}

function lastError(output) {
  const lines = output.trim().split('\n').reverse();
  const line = lines.find((l) => /^\w*Error: |^Test262Error: |^\w+: /.test(l.trim())) ?? '';
  const match = line.trim().match(/^(\w+): (.*)$/);
  return match ? { error_name: match[1], message: match[2].slice(0, 300) } : {};
}

// /proc can belong to an outer PID namespace (gVisor sandboxes): there a child's pid names some other process,
// and the child's entry is the one whose parent is this process's entry and with `marker` among its arguments.
const SELF_PROC = (() => {
  try {
    return fs.readlinkSync('/proc/self');
  } catch {
    return String(process.pid);
  }
})();

function procEntryOf(child, marker) {
  if (SELF_PROC === String(process.pid)) return String(child.pid);
  for (const entry of fs.readdirSync('/proc')) {
    if (!/^\d+$/.test(entry)) continue;
    try {
      const stat = fs.readFileSync(`/proc/${entry}/stat`, 'utf8');
      const ppid = stat.slice(stat.lastIndexOf(')') + 2).split(' ')[1];
      if (ppid === SELF_PROC && fs.readFileSync(`/proc/${entry}/cmdline`, 'utf8').split('\0').includes(marker)) return entry;
    } catch {
      // gone meanwhile
    }
  }
  return null;
}

// A process's resident set in KB (0 once it has exited).
function rssKb(entry) {
  try {
    const match = /^VmRSS:\s+(\d+) kB/m.exec(fs.readFileSync(`/proc/${entry}/status`, 'utf8'));
    return match ? Number(match[1]) : 0;
  } catch {
    return 0;
  }
}

// The harness file that threw and its error (the line after the marker), as one message.
function hostScriptError(output) {
  const lines = output.split('\n');
  const at = lines.findIndex((l) => l.startsWith(HOST_SCRIPT_ERROR));
  return `${lines[at].slice(HOST_SCRIPT_ERROR.length).trim()}: ${lines[at + 1] ?? ''}`.slice(0, 300);
}

// Runs the command under an address-space cap of vmLimitKb (0: none); SIGKILLs it after timeoutMs, or once its
// resident set exceeds memLimitKb. marker is an argument unique to this run among concurrent ones (a temp
// file's path).
function execute(command, args, timeoutMs, memLimitKb, vmLimitKb, marker) {
  return new Promise((resolve) => {
    const started = performance.now();
    const capped = vmLimitKb ? ['/bin/sh', ['-c', 'ulimit -v "$0" && exec "$@"', String(vmLimitKb), command, ...args]] : [command, args];
    const child = spawn(capped[0], capped[1], { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '';
    let timedOut = false;
    let memoryExceeded = false;
    const timer = setTimeout(() => {
      timedOut = true;
      child.kill('SIGKILL');
    }, timeoutMs);
    let entry = null;
    const watchdog = setInterval(() => {
      if (child.exitCode !== null || child.signalCode !== null) return;
      entry ??= procEntryOf(child, marker);
      if (entry && rssKb(entry) > memLimitKb) {
        memoryExceeded = true;
        child.kill('SIGKILL');
      }
    }, MEM_POLL_MS);
    const stop = () => {
      clearTimeout(timer);
      clearInterval(watchdog);
    };
    child.stdout.on('data', (d) => (output += d));
    child.stderr.on('data', (d) => (output += d));
    child.on('exit', stop);
    child.on('close', (code) => {
      stop();
      resolve({ code: code ?? 1, output, timedOut, memoryExceeded, ms: Math.round(performance.now() - started) });
    });
    child.on('error', (error) => {
      stop();
      resolve({ code: 127, output: String(error), timedOut: false, memoryExceeded: false, ms: 0 });
    });
  });
}

async function runScenario(args, engine, scenario, slot) {
  const file = path.join(args.test262, scenario.file);
  const source = fs.readFileSync(file, 'utf8');
  const metadata = metadataOf(source);
  const isModule = metadata.flags.includes('module');
  const engineArgs = [...engine.slice(1)];
  if (args.parseOnly) engineArgs.push('--parse-only');
  const temps = [];
  let entry = file;
  if (!isModule) {
    // Written next to the test, so its relative imports and fixtures resolve.
    entry = path.join(path.dirname(file), `.t262-${process.pid}-${slot}.js`);
    temps.push(entry);
    fs.writeFileSync(entry, compose(args.test262, source, metadata, scenario.mode));
  } else {
    if (!args.parseOnly && !metadata.flags.includes('raw')) {
      const base = path.join(os.tmpdir(), `t262-${process.pid}-${slot}`);
      temps.push(`${base}-prelude.js`, `${base}-epilogue.js`);
      fs.writeFileSync(temps[0], harnessOf(args.test262, metadata));
      fs.writeFileSync(temps[1], `print(${JSON.stringify(RAN)});\n`);
      engineArgs.push('--prelude', temps[0], '--epilogue', temps[1]);
    }
    engineArgs.push('--module');
  }
  engineArgs.push(entry);
  // An argument no concurrent scenario has: the temp file, or the module test itself (it runs in one mode).
  const marker = temps[0] ?? entry;
  const result = await execute(engine[0], engineArgs, TIMEOUT_MS, Number(args.memMb) * 1024, Number(args.vmMb) * 1024, marker);
  for (const temp of temps) fs.rmSync(temp, { force: true });

  const status = verdict(result, metadata, args.parseOnly);
  const failure = result.memoryExceeded
    ? { error_name: 'MemoryLimit', message: `killed: resident memory above ${args.memMb} MB` }
    : status === 'pass'
      ? {}
      : result.output.includes(HOST_SCRIPT_ERROR)
        ? { error_name: 'HostScriptError', message: hostScriptError(result.output) }
        : lastError(result.output);
  return { file: scenario.file, mode: scenario.mode, status, ms: result.ms, ...failure };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const [k, n] = args.shard.split('/').map(Number);
  const engine = args.engine.split(/\s+/).filter(Boolean);
  const all = listScenarios(args.test262, args.suite);
  const mine = all.filter((s) => shardOf(s, n) === k && (!args.filter || s.file.startsWith(args.filter)));

  const out = args.out ? fs.createWriteStream(args.out) : process.stdout;
  let next = 0;
  let passed = 0;
  const started = performance.now();
  async function worker(slot) {
    while (next < mine.length) {
      const scenario = mine[next++];
      const line = await runScenario(args, engine, scenario, slot);
      if (line.status === 'pass') passed++;
      out.write(JSON.stringify(line) + '\n');
    }
  }
  await Promise.all(Array.from({ length: Number(args.jobs) }, (_, slot) => worker(slot)));
  if (args.out) await new Promise((resolve) => out.end(resolve));

  const seconds = ((performance.now() - started) / 1000).toFixed(1);
  console.error(JSON.stringify({ suite: args.suite, shard: args.shard, scenarios: mine.length, passed, seconds: Number(seconds) }));
}

await main();
