// aggregate.mjs: merge shard JSONL into one results file, after checking every scenario of the suite
// was run exactly once. A file passes only if all its scenarios pass (test262.fyi's rule).
//
//   node runner/aggregate.mjs --suite es-core --label <engine sha> [--baseline results/<old>.json]
//                             --out results/<sha>.json shard-0.jsonl shard-1.jsonl ...
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { listScenarios, metadataOf } from './scenarios.mjs';

const KIT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function parseArgs(argv) {
  const args = { suite: 'es-core', test262: path.join(KIT, 'test262'), shards: [] };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i].startsWith('--')) {
      args[argv[i].slice(2)] = argv[++i];
      continue;
    }
    args.shards.push(argv[i]);
  }
  return args;
}

function readLines(files) {
  const lines = [];
  for (const file of files) {
    for (const text of fs.readFileSync(file, 'utf8').split('\n')) {
      if (text.trim()) lines.push(JSON.parse(text));
    }
  }
  return lines;
}

// completeness compares what ran with what the suite has.
function completeness(expected, lines) {
  const seen = new Map();
  for (const line of lines) {
    const key = `${line.file}#${line.mode}`;
    seen.set(key, (seen.get(key) ?? 0) + 1);
  }
  const keys = new Set(expected.map((s) => `${s.file}#${s.mode}`));
  const missing = [...keys].filter((key) => !seen.has(key));
  const duplicated = [...seen].filter(([, count]) => count > 1).map(([key]) => key);
  const unexpected = [...seen.keys()].filter((key) => !keys.has(key));
  return { expected: keys.size, ran: lines.length, missing, duplicated, unexpected };
}

// rollup counts files and passing files at every directory depth.
function rollup(lines) {
  const fileStatus = new Map();
  for (const line of lines) {
    const passing = fileStatus.get(line.file) ?? true;
    fileStatus.set(line.file, passing && line.status === 'pass');
  }
  const dirs = {};
  for (const [file, passing] of fileStatus) {
    const parts = path.dirname(file).split(path.sep);
    for (let depth = 1; depth <= parts.length; depth++) {
      const dir = parts.slice(0, depth).join('/');
      dirs[dir] ??= { files: 0, passed: 0 };
      dirs[dir].files += 1;
      if (passing) dirs[dir].passed += 1;
    }
  }
  const files = fileStatus.size;
  const passed = [...fileStatus.values()].filter(Boolean).length;
  return { files, passed, dirs, fileStatus };
}

// buckets are the file groups the milestone ladder reads that directories don't give.
function buckets(test262, fileStatus) {
  const groups = { negative_parse: { files: 0, passed: 0 }, positive: { files: 0, passed: 0 }, es5_language: { files: 0, passed: 0 } };
  const count = (group, passing) => {
    groups[group].files += 1;
    if (passing) groups[group].passed += 1;
  };
  for (const [file, passing] of fileStatus) {
    const metadata = metadataOf(fs.readFileSync(path.join(test262, file), 'utf8'));
    const parseNegative = metadata.negative && (metadata.negative.phase === 'parse' || metadata.negative.phase === 'resolution');
    count(parseNegative ? 'negative_parse' : 'positive', passing);
    // The ES5 interpreter bucket: language files with no feature tag that must run, not fail to parse.
    if (file.startsWith('test/language/') && metadata.features.length === 0 && !parseNegative) count('es5_language', passing);
  }
  return groups;
}

function diff(baseline, fileStatus) {
  if (!baseline?.passing_files) return null;
  const before = new Set(baseline.passing_files);
  const now = [...fileStatus].filter(([, passing]) => passing).map(([file]) => file);
  const nowSet = new Set(now);
  return {
    newly_passing: now.filter((file) => !before.has(file)).length,
    regressions: [...before].filter((file) => !nowSet.has(file)),
  };
}

const args = parseArgs(process.argv.slice(2));
const expected = listScenarios(args.test262, args.suite);
const lines = readLines(args.shards);
const check = completeness(expected, lines);
const { files, passed, dirs, fileStatus } = rollup(lines);
const baseline = args.baseline ? JSON.parse(fs.readFileSync(args.baseline, 'utf8')) : null;

const result = {
  suite: args.suite,
  label: args.label ?? null,
  test262: fs.readFileSync(path.join(args.test262, '.git', 'HEAD'), 'utf8').trim(),
  scenarios: { expected: check.expected, ran: check.ran, passed: lines.filter((l) => l.status === 'pass').length },
  complete: check.missing.length === 0 && check.duplicated.length === 0 && check.unexpected.length === 0,
  files: { total: files, passed, percent: Number(((100 * passed) / Math.max(files, 1)).toFixed(2)) },
  buckets: buckets(args.test262, fileStatus),
  dirs,
  passing_files: [...fileStatus].filter(([, passing]) => passing).map(([file]) => file).sort(),
  diff: diff(baseline, fileStatus),
};
if (args.out) fs.writeFileSync(args.out, JSON.stringify(result, null, 1));
const summary = { ...result, dirs: undefined, passing_files: undefined, missing: check.missing.length, duplicated: check.duplicated.length, unexpected: check.unexpected.length };
console.log(JSON.stringify(summary));
if (!result.complete) process.exitCode = 2;
