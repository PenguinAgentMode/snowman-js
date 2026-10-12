// run.mjs: run one shard of a test262 suite against an engine, one process per scenario, and write
// a JSONL line per scenario. Tests run in place, so module specifiers resolve as INTERPRETING.md says.
//
//   node runner/run.mjs --engine "<command>" [--suite es-core|full] [--shard k/N] [--jobs J]
//                       [--parse-only] [--filter <path prefix>] [--out results/shard.jsonl]
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { listScenarios, metadataOf, shardOf } from './scenarios.mjs';

const KIT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const TIMEOUT_MS = 10_000;
const DONOTEVALUATE = 'Test262: This statement should not be evaluated.';
// RAN is printed after a test's body, so an engine that runs nothing and exits 0 can't pass it.
const RAN = 'Test262:Ran';

function parseArgs(argv) {
  const args = { suite: 'es-core', shard: '0/1', jobs: os.cpus().length, parseOnly: false, test262: path.join(KIT, 'test262') };
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

// compose is the source a scenario runs: the harness, the test's includes, then the test.
function compose(test262, source, metadata, mode) {
  const strict = mode === 'strict' ? '"use strict";\n' : '';
  if (metadata.flags.includes('raw')) return strict + source;

  const prelude = [harness(test262, 'assert.js'), harness(test262, 'sta.js')];
  if (metadata.flags.includes('async')) prelude.push(harness(test262, 'doneprintHandle.js'));
  for (const include of metadata.includes) prelude.push(harness(test262, include));
  return strict + prelude.join('\n') + '\n' + source + `\n;print(${JSON.stringify(RAN)});\n`;
}

// verdict decides a scenario from the engine's exit code and output.
function verdict({ code, output, timedOut }, metadata, parseOnly) {
  if (timedOut) return 'timeout';
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

function execute(command, args, timeoutMs) {
  return new Promise((resolve) => {
    const started = performance.now();
    const child = spawn(command, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '';
    let timedOut = false;
    const timer = setTimeout(() => {
      timedOut = true;
      child.kill('SIGKILL');
    }, timeoutMs);
    child.stdout.on('data', (d) => (output += d));
    child.stderr.on('data', (d) => (output += d));
    child.on('close', (code) => {
      clearTimeout(timer);
      resolve({ code: code ?? 1, output, timedOut, ms: Math.round(performance.now() - started) });
    });
    child.on('error', (error) => {
      clearTimeout(timer);
      resolve({ code: 127, output: String(error), timedOut: false, ms: 0 });
    });
  });
}

async function runScenario(args, engine, scenario, slot) {
  const file = path.join(args.test262, scenario.file);
  const source = fs.readFileSync(file, 'utf8');
  const metadata = metadataOf(source);
  const isModule = metadata.flags.includes('module');
  // Written next to the test, so its relative imports and fixtures resolve.
  const composed = path.join(path.dirname(file), `.t262-${process.pid}-${slot}${isModule ? '.mjs' : '.js'}`);
  fs.writeFileSync(composed, compose(args.test262, source, metadata, scenario.mode));

  const engineArgs = [...engine.slice(1)];
  if (args.parseOnly) engineArgs.push('--parse-only');
  if (isModule) engineArgs.push('--module');
  engineArgs.push(composed);
  const result = await execute(engine[0], engineArgs, TIMEOUT_MS);
  fs.rmSync(composed, { force: true });

  const status = verdict(result, metadata, args.parseOnly);
  const failure = status === 'pass' ? {} : lastError(result.output);
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
