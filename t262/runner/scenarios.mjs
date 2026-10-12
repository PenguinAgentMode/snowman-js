// Scenarios: every (test file, mode) a suite runs, in a deterministic order. A file runs in both
// non-strict and strict mode unless its flags say otherwise (INTERPRETING.md).
import fs from 'node:fs';
import path from 'node:path';

// ES-core leaves out Temporal, Intl, staging and proposals: the denominator the ladder uses.
const ES_CORE_ROOTS = ['test/language', 'test/built-ins', 'test/annexB', 'test/harness'];
const FULL_ROOTS = [...ES_CORE_ROOTS, 'test/intl402', 'test/staging'];
const ES_CORE_EXCLUDED_DIRS = ['test/built-ins/Temporal'];

export const SUITES = { 'es-core': ES_CORE_ROOTS, full: FULL_ROOTS };

// listOf reads a YAML list in either form: inline "key: [a, b]" or a block of "- a" lines.
function listOf(block, key) {
  const inline = block.match(new RegExp(`^${key}:\\s*\\[(.*)\\]`, 'm'));
  if (inline) return inline[1].split(',').map((s) => s.trim()).filter(Boolean);
  const blockList = block.match(new RegExp(`^${key}:\\s*\\n((?:\\s+-\\s*.+\\n?)+)`, 'm'));
  if (!blockList) return [];
  return blockList[1].split('\n').map((line) => line.replace(/^\s*-\s*/, '').trim()).filter(Boolean);
}

// metadataOf reads a test's frontmatter: the YAML between /*--- and ---*/.
export function metadataOf(source) {
  const block = source.match(/\/\*---([\s\S]*?)---\*\//)?.[1] ?? '';
  const list = (key) => listOf(block, key);
  const negative = block.match(/negative:\s*\n\s+phase:\s*(\w+)\s*\n\s+type:\s*(\w+)/);
  return {
    flags: list('flags'),
    includes: list('includes'),
    features: list('features'),
    negative: negative ? { phase: negative[1], type: negative[2] } : null,
  };
}

// modesOf is the modes a test runs in.
export function modesOf(flags) {
  if (flags.includes('onlyStrict')) return ['strict'];
  const runsOnce = flags.includes('noStrict') || flags.includes('raw') || flags.includes('module');
  if (runsOnce) return ['sloppy'];
  return ['sloppy', 'strict'];
}

function isTestFile(name) {
  return name.endsWith('.js') && !name.includes('_FIXTURE');
}

function walk(dir, out) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      walk(full, out);
      continue;
    }
    if (isTestFile(entry.name)) out.push(full);
  }
}

// proposalFeatures are the features.txt tags under "## Proposals" (or above Standard): ES-core excludes them.
function proposalFeatures(test262Dir) {
  const text = fs.readFileSync(path.join(test262Dir, 'features.txt'), 'utf8');
  const proposals = new Set();
  let inProposals = true;
  for (const line of text.split('\n')) {
    if (/^##\s*Standard/i.test(line)) inProposals = false;
    if (/^##\s*Propos/i.test(line)) inProposals = true;
    const feature = line.trim();
    if (inProposals && feature && !feature.startsWith('#')) proposals.add(feature);
  }
  return proposals;
}

// listScenarios returns the suite's scenarios, sorted by file then mode.
export function listScenarios(test262Dir, suite) {
  const roots = SUITES[suite];
  if (!roots) throw new Error(`unknown suite ${suite}; one of ${Object.keys(SUITES).join(', ')}`);
  const files = [];
  for (const root of roots) walk(path.join(test262Dir, root), files);

  const proposals = suite === 'es-core' ? proposalFeatures(test262Dir) : new Set();
  const scenarios = [];
  for (const file of files) {
    const relative = path.relative(test262Dir, file);
    if (suite === 'es-core' && ES_CORE_EXCLUDED_DIRS.some((dir) => relative.startsWith(dir + path.sep))) continue;
    const metadata = metadataOf(fs.readFileSync(file, 'utf8'));
    const leftOutOfCore = suite === 'es-core' && metadata.features.some((feature) => proposals.has(feature) || feature === 'Temporal');
    if (leftOutOfCore) continue;
    for (const mode of modesOf(metadata.flags)) scenarios.push({ file: relative, mode });
  }
  return scenarios;
}

// shardOf assigns a scenario to one of n shards by a stable hash of its key.
export function shardOf(scenario, n) {
  let hash = 2166136261;
  for (const char of `${scenario.file}#${scenario.mode}`) {
    hash ^= char.charCodeAt(0);
    hash = Math.imul(hash, 16777619);
  }
  return (hash >>> 0) % n;
}
