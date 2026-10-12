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

// Where the spec and V8 disagree; each entry is checked to be a real disagreement, and reported apart.
//   Katakana_Or_Hiragana/Hrkt: in PropertyValueAliases.txt, so valid; V8 rejects property values with no code points.
//   WSpace: an alias in PropertyAliases.txt but not in ECMA-262's table 68, which forbids other aliases; V8 takes it.
// An independent check of the spec's duplicate-name rule: true if two groups with the same name might both
// participate (MightBothParticipate: no Disjunction has one in an Alternative and the other in the rest). V8 is
// more lenient: after a | it forgets names from outside that disjunction, e.g. (?<a>x)(?:z|(?<a>y)), and it takes
// (?<a>(?<a>x)|y).
function specDuplicateNames(p, flags) {
  const v = flags.includes('v');
  const stack = [[0, 0]];  // [disjunction id, alternative index]
  let ids = 1;
  const groups = [];
  let depth = 0;
  for (let i = 0; i < p.length; i++) {
    const c = p[i];
    if (c === '\\') {
      i++;
    } else if (depth) {
      if (c === ']') depth--;
      else if (c === '[' && v) depth++;
    } else if (c === '[') {
      depth = 1;
    } else if (c === '(') {
      const m = /^\(\?<([^=!>][^>]*)>/.exec(p.slice(i));
      if (m) groups.push({ name: unescapeName(m[1]), path: stack.map((e) => [...e]) });
      stack.push([ids++, 0]);
    } else if (c === '|') {
      stack[stack.length - 1][1]++;
    } else if (c === ')') {
      stack.pop();
    }
  }
  const both = (a, b) => {
    for (let k = 0; k < Math.min(a.length, b.length); k++) {
      if (a[k][0] !== b[k][0] || a[k][1] !== b[k][1]) return !(a[k][0] === b[k][0]);
    }
    return true;
  };
  for (let x = 0; x < groups.length; x++)
    for (let y = x + 1; y < groups.length; y++) if (groups[x].name === groups[y].name && both(groups[x].path, groups[y].path)) return true;
  return false;
}
function unescapeName(raw) {
  return raw.replace(/\\u\{([0-9a-fA-F]+)\}|\\u([0-9a-fA-F]{4})/g, (_, a, b) => String.fromCharCode(...codeUnits(parseInt(a ?? b, 16))));
}
function codeUnits(cp) {
  return cp > 0xffff ? [0xd800 + ((cp - 0x10000) >> 10), 0xdc00 + ((cp - 0x10000) & 0x3ff)] : [cp];
}

const KNOWN = [
  { why: 'duplicate names that might both participate (spec: SyntaxError, V8 accepts some)', test: (p, f, ours) => ours.includes('Duplicate capture group name') && specDuplicateNames(p, f) },
  { why: 'sc/scx=Hrkt (spec: valid, V8: rejects empty values)', test: (p) => /\\[pP]\{(sc|scx|Script|Script_Extensions)=(Hrkt|Katakana_Or_Hiragana)\}/.test(p) },
  { why: 'WSpace (spec table 68 lists only White_Space/space)', test: (p) => /\\[pP]\{WSpace\}/.test(p) },
];

const nodeVerdict = (pattern, flags) => {
  try {
    new RegExp(pattern, flags);
    return true;
  } catch (e) {
    if (!(e instanceof SyntaxError)) throw e;
    return false;
  }
};

// ---- fuzzing: token soup, biased towards RegExp syntax ----
function rng(seed) {
  let s = seed >>> 0 || 1;
  return () => {
    s ^= s << 13;
    s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5;
    s >>>= 0;
    return s / 4294967296;
  };
}
const TOKENS = [
  'a', 'b', 'k', 'p', 'P', 'u', 'x', 'c', 'd', 'q', 'i', 'm', 's', 'Z', '0', '1', '2', '7', '8', '9', '_', '-', '^', '$', '.',
  '*', '+', '?', '|', '(', ')', '(', ')', '[', ']', '[', ']', '{', '}', ',', '<', '>', '=', '!', ':', '&', '\\', '/', '#', '~',
  '%', ';', '@', '`', ' ', '\u00e9', '\u212a', '\u017f', '\u{1F600}', '\uD83D', '\uDE00', '\u0130', '\u200c',
  '(?<a>', '(?<b>', '(?<$>', '(?<\\u0061>', '(?<\\u{62}>', '(?<a1>', '(?<1>', '(?<π>', '(?<\u{1d4d1}>', '\\k<a>', '\\k<b>',
  '\\k', '(?:', '(?=', '(?!', '(?<=', '(?<!', '(?i:', '(?-i:', '(?m-s:', '(?ims:', '(?i-i:', '(?-:', '(?x:', '(?ii:',
  '\\p{L}', '\\p{Lu}', '\\P{Lu}', '\\p{gc=Lu}', '\\p{General_Category=L}', '\\p{Script=Greek}', '\\p{scx=Grek}',
  '\\p{sc=Latn}', '\\p{ASCII}', '\\p{Any}', '\\p{RGI_Emoji}', '\\P{RGI_Emoji}', '\\p{Basic_Emoji}', '\\p{Emoji_Keycap_Sequence}',
  '\\p{Lowercase}', '\\p{lu}', '\\p{Latin}', '\\p{}', '\\p{L', '\\p{ASCII=Y}', '\\p{Script}', '\\q{abc|d}', '\\q{a}',
  '\\q{}', '\\q{', '[^', '&&', '--', '&&&', '!!', '##', '\\u{1F600}', '\\u{110000}', '\\u{}', '\\uD83D\\uDE00', '\\uD83D',
  '\\u0041', '\\u004', '\\x41', '\\x4', '\\cA', '\\cz', '\\c1', '\\c_', '\\c', '{1}', '{2,}', '{1,3}', '{3,1}', '{1,',
  '{,1}', '{99999999999,1}', '\\b', '\\B', '\\d', '\\D', '\\w', '\\W', '\\s', '\\S', '\\1', '\\2', '\\10', '\\01', '\\00',
  '\\0', '\\8', '\\-', '\\/', '\\]', '\\[', '\\.', '\\a', '\\e', '\\_', '\\&', '\\!', '\\q', '\\$', '\\{', '\\}', '\\|',
];
const FLAGS = ['', '', '', 'u', 'u', 'v', 'v', 'i', 'iu', 'iv', 'g', 'gimsy', 'dv', 'mu', 'uv', 'gg', 'x', 'su'];
function fuzz(n, seed) {
  const r = rng(seed);
  const cases = [];
  for (let k = 0; k < n; k++) {
    const len = 1 + Math.floor(r() * 10);
    let p = '';
    for (let t = 0; t < len; t++) p += TOKENS[Math.floor(r() * TOKENS.length)];
    cases.push({ pattern: p, flags: FLAGS[Math.floor(r() * FLAGS.length)], kind: 'fuzz' });
  }
  return cases;
}

// ---- fuzzing: grammar-directed patterns (mostly valid), then mutated ----
const PROPS = [
  'L', 'Lu', 'Ll', 'LC', 'Cased_Letter', 'punct', 'digit', 'Nd', 'Zs', 'Cn', 'Co', 'Cs', 'Any', 'ASCII', 'Assigned', 'Alpha',
  'White_Space', 'space', 'WSpace', 'Emoji', 'ExtPict', 'IDS', 'XIDC', 'Lowercase', 'Upper', 'RI', 'gc=Lu', 'gc=L',
  'General_Category=Letter', 'sc=Grek', 'Script=Greek', 'scx=Latn', 'Script_Extensions=Latin', 'sc=Zyyy', 'sc=Qaai',
  'sc=Hrkt', 'scx=Katakana_Or_Hiragana', 'sc=Unknown', 'Script=Common', 'RGI_Emoji', 'Basic_Emoji', 'RGI_Emoji_ZWJ_Sequence',
  'RGI_Emoji_Flag_Sequence', 'Emoji_Keycap_Sequence', 'RGI_Emoji_Tag_Sequence', 'RGI_Emoji_Modifier_Sequence', 'Latin',
  'Greek', 'L&', 'lu', 'gc=lu', 'Lu=gc', 'Block=Basic_Latin', 'InBasic_Latin', 'IsLatin', 'ASCII=Y', 'sc', 'gc', 'Hex',
  'IDS_Unary_Operator', 'ID_Compat_Math_Start', 'sc=Jurc', 'Garay', 'sc=Gara', 'scx=Gara', 'sc=Tols', 'Script=Sidetic',
];
function grammarFuzz(n, seed) {
  const r = rng(seed);
  const pick = (a) => a[Math.floor(r() * a.length)];
  const chance = (p) => r() < p;
  const chars = ['a', 'b', 'Z', 'k', '_', '0', '9', ' ', '-', ',', '=', '!', '#', '&', '~', '%', '\u00e9', '\u212a', '\u{1F600}', '\uD83D', '\uDE00'];
  let names;
  function classAtom(v) {
    if (chance(0.2)) return `\\${pick(['d', 'D', 'w', 'W', 's', 'S'])}`;
    if (chance(0.15)) return `\\${pick(['p', 'P'])}{${pick(PROPS)}}`;
    if (chance(0.15)) return `\\${pick(['n', 'b', 'x41', 'u0041', 'u{1F600}', 'cA', 'c1', '0', '-', ']', '^', '/', '&', '!', '1', '8'])}`;
    if (v && chance(0.15)) return `\\q{${Array.from({ length: Math.floor(r() * 3) }, () => pick(['', 'a', 'ab', '\u{1F600}', 'xyz'])).join('|')}}`;
    if (v && chance(0.15)) return classV(1);
    return pick(chars);
  }
  function classV(depth) {
    const neg = chance(0.2) ? '^' : '';
    const k = Math.floor(r() * 3);
    if (k === 0 || depth > 2) {
      let s = '';
      for (let i = Math.floor(r() * 4); i > 0; i--) s += chance(0.2) ? `${pick(chars)}-${pick(chars)}` : classAtom(true);
      return `[${neg}${s}]`;
    }
    const op = k === 1 ? '&&' : '--';
    const parts = Array.from({ length: 2 + Math.floor(r() * 2) }, () => (chance(0.5) ? classV(depth + 1) : classAtom(true)));
    return `[${neg}${parts.join(op)}]`;
  }
  function classRanges() {
    let s = chance(0.2) ? '^' : '';
    for (let i = Math.floor(r() * 5); i > 0; i--) s += chance(0.3) ? `${classAtom(false)}-${classAtom(false)}` : classAtom(false);
    return `[${s}]`;
  }
  function quant() {
    const q = pick(['', '', '', '*', '+', '?', '{2}', '{1,}', '{0,3}', '{3,2}', '{', '{1', '{,2}']);
    return q && chance(0.3) ? q + '?' : q;
  }
  function atom(depth, v) {
    const k = r();
    if (k < 0.3) return pick(chars.concat(['.', '^', '$', '\\b', '\\B', ']', '}', '{']));
    if (k < 0.45) return `\\${pick(['d', 'w', 's', 'n', 't', 'x41', 'u0041', 'u{41}', 'uD83D\\uDE00', 'cJ', 'c', '0', '00', '1', '2', '10', '8', 'k', '/', '-', 'a', '.', '*'])}`;
    if (k < 0.5) return `\\${pick(['p', 'P'])}{${pick(PROPS)}}`;
    if (k < 0.62) return v ? classV(0) : classRanges();
    if (k < 0.67 && names.length) return `\\k<${pick(names)}>`;
    if (depth > 3) return 'x';
    const body = disjunction(depth + 1, v);
    const g = r();
    if (g < 0.3) return `(${body})`;
    if (g < 0.45) {
      const name = pick(['a', 'b', 'a', '$x', 'π', '\\u0061', '\\u{62}', '\u{1d4d1}', '\\uD835\\uDCD1', '1a']);
      names.push(name);
      return `(?<${name}>${body})`;
    }
    if (g < 0.6) return `(?${pick(['=', '!', '<=', '<!'])}${body})`;
    if (g < 0.75) return `(?${pick(['', 'i', 'm', 's', 'ims', '-i', 'i-m', 'm-is', '-', 'ii', 'i-i', 'x', 'I'])}:${body})`;
    return `(?:${body})`;
  }
  function alternative(depth, v) {
    let s = '';
    for (let i = Math.floor(r() * 4); i > 0; i--) s += atom(depth, v) + quant();
    return s;
  }
  function disjunction(depth, v) {
    const parts = [alternative(depth, v)];
    while (chance(0.3)) parts.push(alternative(depth, v));
    return parts.join('|');
  }
  const out = [];
  for (let i = 0; i < n; i++) {
    names = [];
    const flags = pick(['', '', 'u', 'u', 'v', 'v', 'i', 'iu', 'iv', 'imsu']);
    let p = disjunction(0, flags.includes('v'));
    if (chance(0.5) && p.length) {
      // one mutation: delete, duplicate or replace a code unit
      const at = Math.floor(r() * p.length);
      const m = r();
      if (m < 0.33) p = p.slice(0, at) + p.slice(at + 1);
      else if (m < 0.66) p = p.slice(0, at) + p[at] + p.slice(at);
      else p = p.slice(0, at) + pick(['(', ')', '[', ']', '{', '}', '|', '\\', '-', '&', '?', '<', '>', 'k', 'q']) + p.slice(at + 1);
    }
    out.push({ pattern: p, flags, kind: 'grammar' });
  }
  return out;
}

// ---- every property name and alias, in every position, with near misses ----
function propertyCases() {
  const inc = readFileSync(path.join(root, 'src/unicode/property_tables.inc'), 'utf8');
  const names = (table) => [...inc.split(`${table}[] = {`)[1].split('};')[0].matchAll(/\{"([^"]+)"/g)].map((m) => m[1]);
  const bin = names('kBinaryNames');
  const all = new Set([...names('kGeneralCategoryNames'), ...names('kScriptNames'), ...bin, ...names('kStringPropertyNames')]);
  for (const extra of ['WSpace', 'IDS_Unary_Operator', 'ID_Compat_Math_Start', 'Block', 'InCB', 'Jurc', 'Seal']) all.add(extra);
  const variants = (n) => [n, n.toLowerCase(), n.toUpperCase(), n.replace(/_/g, ''), n.replace(/_/g, ' '), n.replace(/_/g, '-'), `Is${n}`, `In${n}`];
  const prefixes = ['', 'gc=', 'General_Category=', 'sc=', 'Script=', 'scx=', 'Script_Extensions=', 'GC=', 'script=', 'Sc='];
  const cases = [];
  for (const n of all)
    for (const x of variants(n))
      for (const pre of prefixes)
        for (const flags of ['u', 'v', '']) {
          cases.push({ pattern: `\\p{${pre}${x}}`, flags, kind: 'property' }, { pattern: `\\P{${pre}${x}}`, flags, kind: 'property' });
          if (flags === 'v') {
            for (const p of [`[^\\p{${pre}${x}}]`, `[\\p{${pre}${x}}--a]`, `[a&&\\p{${pre}${x}}]`]) cases.push({ pattern: p, flags, kind: 'property' });
          }
        }
  for (const n of bin) for (const val of ['Y', 'Yes', 'T', 'True', 'N', 'No', 'F', 'False']) cases.push({ pattern: `\\p{${n}=${val}}`, flags: 'u', kind: 'property' });
  return cases;
}

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
