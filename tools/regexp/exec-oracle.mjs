// exec-oracle.mjs: differential test of snowman::regexp::compile/exec against node's RegExp.prototype.exec.
//   node tools/regexp/exec-oracle.mjs [--cases cases.jsonl] [--patterns N] [--grammar N] [--inputs K] [--seed S] [--show K]
// Patterns come from extract.mjs output (--cases: test262's literals), gen.mjs's execPatterns (--patterns, the
// default: 20000) and/or the validator's grammar fuzzer (--grammar), keeping those node accepts; each runs on K generated inputs over an alphabet taken from the pattern (its characters, their
// case variants, the characters its escapes name) plus case-folding and surrogate edge cases, at a lastIndex
// with g or y. Captures are compared as spans, node's from the d flag.
import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';

import { runExecDriver } from './driver.mjs';
import { KNOWN_SYNTAX, execPatterns, grammarFuzz, rng } from './gen.mjs';

const NODE_FLAGS = ['--js-regexp-modifiers', '--js-regexp-duplicate-named-groups'];
try {
  new RegExp('(?i:a)');
} catch {
  const r = spawnSync(process.execPath, [...NODE_FLAGS, ...process.argv.slice(1)], { stdio: 'inherit' });
  process.exit(r.status ?? 1);
}

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(`--${name}`);
  return i < 0 ? dflt : args[i + 1];
};

const BASE = ['a', 'A', 'b', 'B', 'z', '0', '9', '_', ' ', '-', '\n', '\r', '\u2028', '\t', 'é', 'É', 'ſ', 'K', 'k', 's', 'S', 'ß',
  'ẞ', 'Ω', 'ω', 'Ω', '\u0130', 'i', 'I', '\u0131', '\ud83d', '\ude00', '\u{1F600}', '\u{10400}', '\u{10428}', '\u{1F1EF}\u{1F1F5}',
  '#\ufe0f\u20e3', '\u{1F468}\u200d\u{1F469}'];
function alphabet(pattern) {
  const chars = new Set(BASE);
  for (const c of pattern) {
    chars.add(c);
    for (const v of [c.toLowerCase(), c.toUpperCase()]) if ([...v].length === 1) chars.add(v);
  }
  for (const m of pattern.matchAll(/\\u\{([0-9a-fA-F]+)\}|\\u([0-9a-fA-F]{4})|\\x([0-9a-fA-F]{2})|\\c([a-zA-Z])/g)) {
    const cp = m[4] ? m[4].charCodeAt(0) % 32 : parseInt(m[1] ?? m[2] ?? m[3], 16);
    if (cp <= 0x10ffff) {
      const s = String.fromCodePoint(cp);
      chars.add(s);
      for (const v of [s.toLowerCase(), s.toUpperCase()]) if ([...v].length === 1) chars.add(v);
    }
  }
  return [...chars];
}

function nodeExec(pattern, flags, input, lastIndex) {
  let re;
  try {
    re = new RegExp(pattern, flags.includes('d') ? flags : `${flags}d`);
  } catch {
    return null;
  }
  re.lastIndex = lastIndex;
  const m = re.exec(input);
  if (!m) return 'N';
  return `M ${m.indices.map((p) => (p ? `${p[0]} ${p[1]}` : '-1 -1')).join(' ')}`;
}

// ---- cases ----
const r = rng(Number(opt('seed', 1)));
const pick = (a) => a[Math.floor(r() * a.length)];
let patterns = [];
const caseFile = opt('cases');
if (caseFile) {
  for (const l of readFileSync(caseFile, 'utf8').trim().split('\n')) {
    const c = JSON.parse(l);
    patterns.push({ pattern: c.pattern, flags: c.flags, file: c.file });
  }
}
const nGrammar = Number(opt('grammar', 0));
if (nGrammar) patterns = patterns.concat(grammarFuzz(nGrammar, Number(opt('seed', 1)) + 11, { mutate: false }));
const nGen = Number(opt('patterns', caseFile || nGrammar ? 0 : 20000));
if (nGen) patterns = patterns.concat(execPatterns(nGen, Number(opt('seed', 1)) + 13));
const seen = new Set();
patterns = patterns.filter((p) => {
  const k = `${p.flags}/${p.pattern}`;
  if (seen.has(k)) return false;
  seen.add(k);
  try {
    new RegExp(p.pattern, p.flags);
    return true;
  } catch {
    return false;
  }
});
const K = Number(opt('inputs', 8));
const cases = [];
for (const p of patterns) {
  const abc = alphabet(p.pattern);
  for (let k = 0; k < K; k++) {
    let input = '';
    const len = Math.floor(r() * 10);
    for (let i = 0; i < len; i++) input += pick(abc);
    const lastIndex = /[gy]/.test(p.flags) ? Math.floor(r() * (input.length + 2)) : 0;
    cases.push({ ...p, input, lastIndex });
  }
}

// ---- compare ----
const ours = runExecDriver(cases);
let agree = 0;
const known = new Map();
const mismatches = [];
const isTrail = (c) => c >= 0xdc00 && c <= 0xdfff;
const isLead = (c) => c >= 0xd800 && c <= 0xdbff;
const KNOWN = [
  {
    // RegExpBuiltinExec: the Match Record starts at lastIndex; V8 reports the pair's start.
    why: 'u/v with lastIndex inside a surrogate pair (spec: index = lastIndex)',
    test: (c) => /[uv]/.test(c.flags) && c.lastIndex > 0 && isTrail(c.input.charCodeAt(c.lastIndex)) && isLead(c.input.charCodeAt(c.lastIndex - 1)),
  },
  {
    // AdvanceStringIndex steps over a whole pair; node 22 can report a match starting inside one (e.g. /\B/u).
    why: 'u/v: node matches at an index inside a surrogate pair',
    test: (c, mine, theirs) => {
      const at = theirs?.startsWith('M') ? Number(theirs.split(' ')[1]) : -1;
      return /[uv]/.test(c.flags) && at > 0 && isTrail(c.input.charCodeAt(at)) && isLead(c.input.charCodeAt(at - 1));
    },
  },
  {
    // With v and i the spec canonicalizes the input character at match time against MaybeSimpleCaseFolding'd
    // sets (and a lone General_Category value is not folded); V8 folds eagerly and differs, see ecma262#3443
    // (/\p{ASCII}/iv must match U+017F). class-oracle.mjs checks these sets against a transcription of the spec.
    why: 'v and i (V8 folds sets eagerly, ecma262#3443)',
    test: (c) => c.flags.includes('v') && (c.flags.includes('i') || /\(\?[ms]*i/.test(c.pattern)),
  },
  {
    // node 22 ships modifiers behind --js-regexp-modifiers, with bugs: in
    // /a|(?i:b|(?-i:c)|d|(?i:e)|f)|g|(?-i:h)|k/ it doesn't match "F" or "D", which test262's
    // regexp-modifiers/nesting-ignoreCase-does-not-affect-alternatives-outside.js requires.
    why: 'pattern has modifiers (node 22: experimental and buggy)',
    test: (c) => /\(\?[ims]*-?[ims]*:/.test(c.pattern.replace(/\(\?:/g, '')),
  },
];
for (let i = 0; i < cases.length; i++) {
  const c = cases[i];
  const theirs = nodeExec(c.pattern, c.flags, c.input, c.lastIndex);
  if (ours[i] === theirs) {
    agree++;
    continue;
  }
  let k = KNOWN.find((x) => x.test(c, ours[i], theirs));
  if (!k && ours[i].startsWith('E')) {
    const s = KNOWN_SYNTAX.find((x) => x.test(c.pattern, c.flags, ours[i].slice(2)));
    if (s) k = { why: `validity: ${s.why}` };
  }
  if (k) {
    known.set(k.why, (known.get(k.why) ?? 0) + 1);
    continue;
  }
  mismatches.push({ ...c, ours: ours[i], node: theirs });
}
const matched = cases.filter((_, i) => ours[i]?.startsWith('M')).length;
console.log(`${patterns.length} patterns x ${K} inputs = ${cases.length} execs (${matched} matches): agree ${agree}, known ${[...known.values()].reduce((a, b) => a + b, 0)}, mismatches ${mismatches.length}`);
for (const [why, n] of known) console.log(`  known: ${why}: ${n}`);
const show = Number(opt('show', 30));
for (const m of mismatches.slice(0, show)) {
  console.log(`  /${JSON.stringify(m.pattern).slice(1, -1)}/${m.flags} on ${JSON.stringify(m.input)} @${m.lastIndex}: node ${m.node}, ours ${m.ours}`);
}
process.exitCode = mismatches.length ? 1 : 0;
