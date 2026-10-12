// class-oracle.mjs: v-mode character classes against a transcription of the spec, where node can't be the oracle
// (with v and i, V8 folds sets eagerly: ecma262#3443).
//   (cd tools/unicode && npm install); node tools/regexp/class-oracle.mjs [--classes N] [--seed S] [--show K]
// Random ClassSetExpressions are printed as patterns and, independently, evaluated per CompileToCharSet (22.2.2.9)
// and CompileAtom for CharacterClass (the strings longest first, then the single characters, then the empty
// string). Each runs anchored, ^C$ and ^C, on inputs near its members, with v and with vi; the matcher must agree.
// CharSets are predicates over code points plus a set of strings, so complements of huge sets stay cheap; scf is
// CaseFolding.txt (statuses C and S) from @unicode/unicode-17.0.0, code point properties come from node's \p.
import { runExecDriver } from './driver.mjs';
import { rng } from './gen.mjs';

const ucd = (p) => import(`../unicode/node_modules/@unicode/unicode-17.0.0/${p}`).then((m) => m.default);
const scfMap = new Map([...(await ucd('Case_Folding/C/code-points.mjs')), ...(await ucd('Case_Folding/S/code-points.mjs'))]);
const scf = (c) => scfMap.get(c) ?? c;
// preimage[y]: every x with scf(x) = y (y itself included when it is a fixed point).
const preimage = new Map();
for (const [from, to] of scfMap) {
  if (!preimage.has(to)) preimage.set(to, [to]);
  preimage.get(to).push(from);
}
const pre = (y) => (scf(y) !== y ? [] : preimage.get(y) ?? [y]);

const cps = (s) => [...s].map((c) => c.codePointAt(0));
const str = (list) => String.fromCodePoint(...list);

// A CharSet: has(cp) for the single characters, strings for the other CharSetElements (keys: the strings).
const set = (has, strings = new Set()) => ({ has, strings });
const ctx = (ignoreCase) => ({
  ignoreCase,
  all: ignoreCase ? (c) => scf(c) === c : () => true,  // AllCharacters with v
  fold(s) {
    if (!ignoreCase) return s;
    return set((y) => pre(y).some((x) => s.has(x)), new Set([...s.strings].map((t) => str(cps(t).map(scf)))));
  },
  complement(s) {
    return set((c) => this.all(c) && !s.has(c));
  },
  canon: ignoreCase ? scf : (c) => c,
});
const union = (a, b) => set((c) => a.has(c) || b.has(c), new Set([...a.strings, ...b.strings]));
const intersect = (a, b) => set((c) => a.has(c) && b.has(c), new Set([...a.strings].filter((t) => b.strings.has(t))));
const subtract = (a, b) => set((c) => a.has(c) && !b.has(c), new Set([...a.strings].filter((t) => !b.strings.has(t))));
// Adds a CharSetElement: a one-character string is a character.
const element = (s, t) => {
  const list = cps(t);
  if (list.length === 1) {
    const c = list[0];
    const old = s.has;
    return set((x) => x === c || old(x), s.strings);
  }
  return set(s.has, new Set([...s.strings, t]));
};

// ---- operands: [text, mayContainStrings, evaluate(cx) -> CharSet, characters to try as input] ----
const isBasicWord = (c) => (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5a) || c === 0x5f || (c >= 0x61 && c <= 0x7a);
const wordChars = (cx) => set((c) => isBasicWord(c) || (cx.ignoreCase && isBasicWord(scf(c))));
const spaceRe = /^\s$/u;
const isSpace = (c) => spaceRe.test(String.fromCodePoint(c));
const CHARS = ['a', 'b', 'k', 's', 'z', 'A', 'K', 'S', 'Z', 'ß', 'ẞ', 'ſ', 'K', 'é', 'É', 'Ω', 'ω', 'Ω', 'ϴ', 'θ', '0', '7', '_', ' ',
  '😀', '𐐀', '𐐨', 'Ā', 'ā', 'ǅ', 'ǆ', 'Ǆ', 'İ', 'ı', 'i', 'I'];
const CHAR_ESCAPES = [['\\-', '-'], ['\\&', '&'], ['\\x41', 'A'], ['\\u{1F600}', '😀'], ['\\u212A', '\u212a'], ['\\q{x}', 'x']];
const RANGES = [['a', 'z'], ['A', 'Z'], ['k', 's'], ['0', '9'], ['Ā', 'č'], ['Ω', 'ω'], ['à', 'ÿ'], ['𐐀', '𐐧'], ['K', 'K']];
const PROPS = ['Lu', 'gc=Lu', 'Ll', 'gc=Ll', 'L', 'Lt', 'ASCII', 'Lowercase', 'Uppercase', 'sc=Greek', 'scx=Latn', 'Cased'];
const STRING_PROPS = ['RGI_Emoji_Flag_Sequence', 'Emoji_Keycap_Sequence', 'Basic_Emoji'];
const Q_STRINGS = ['a', 'ab', 'ss', 'SS', 'ß', '', 'kK', 'é', '😀', 'ABC', 'Ab', 'st'];
const stringProps = new Map();
for (const p of STRING_PROPS) stringProps.set(p, await ucd(`Sequence_Property/${p}/index.mjs`));

function propertySet(name) {
  if (stringProps.has(name)) {
    let s = set(() => false);
    for (const t of stringProps.get(name)) s = element(s, t);
    const singles = new Set(stringProps.get(name).filter((t) => cps(t).length === 1).map((t) => cps(t)[0]));
    return set((c) => singles.has(c), s.strings);
  }
  const re = new RegExp(`^\\p{${name}}$`, 'u');
  return set((c) => re.test(String.fromCodePoint(c)));
}

function generator(r) {
  const pick = (a) => a[Math.floor(r() * a.length)];
  const chance = (p) => r() < p;
  function operand(depth) {
    const k = r();
    if (k < 0.3) {
      const [text, ch] = chance(0.8) ? [pick(CHARS), null] : pick(CHAR_ESCAPES);
      const c = cps(ch ?? text)[0];
      return { text, may: false, ev: (cx) => cx.fold(set((x) => x === c)), tries: [c], char: c };
    }
    if (k < 0.42) {
      const e = pick(['d', 'D', 's', 'S', 'w', 'W']);
      const ev = (cx) => {
        const base = e === 'd' || e === 'D' ? set((c) => c >= 0x30 && c <= 0x39) : e === 's' || e === 'S' ? set(isSpace) : cx.fold(wordChars(cx));
        return e === e.toUpperCase() ? cx.complement(base) : base;
      };
      return { text: `\\${e}`, may: false, ev, tries: [0x31, 0x20, 0x61, 0x17f, 0x212a] };
    }
    if (k < 0.6) {
      const strings = chance(0.25);
      const name = strings ? pick(STRING_PROPS) : pick(PROPS);
      const neg = !strings && chance(0.3);
      const lone = !name.includes('=') && /^(L[ult]?|Lt)$/.test(name);
      const ev = (cx) => {
        const base = propertySet(name);
        const s = lone ? base : cx.fold(base);
        return neg ? cx.complement(s) : s;
      };
      const tries = strings ? stringProps.get(name).slice(0, 3).map((t) => cps(t)) : [];
      return { text: `\\${neg ? 'P' : 'p'}{${name}}`, may: strings, ev, tries: tries.filter((t) => t.length === 1).map((t) => t[0]), strings: strings ? stringProps.get(name).slice(0, 3) : [] };
    }
    if (k < 0.72) {
      const list = Array.from({ length: 1 + Math.floor(r() * 3) }, () => pick(Q_STRINGS));
      const ev = (cx) => cx.fold(list.reduce((s, t) => element(s, t), set(() => false)));
      return { text: `\\q{${list.join('|')}}`, may: list.some((t) => cps(t).length !== 1), ev, tries: list.flatMap(cps), strings: list };
    }
    if (depth < 3) return nested(depth + 1);
    return operand(depth);
  }
  function nested(depth) {
    const e = expression(depth);
    const neg = !e.may && chance(0.3);
    return { ...e, text: `[${neg ? '^' : ''}${e.text}]`, may: neg ? false : e.may, ev: neg ? (cx) => cx.complement(e.ev(cx)) : e.ev };
  }
  function expression(depth) {
    const k = r();
    if (k < 0.25 || k < 0.45) {
      const op = k < 0.25 ? '&&' : '--';
      const parts = Array.from({ length: 2 + Math.floor(r() * 2) }, () => operand(depth));
      const combine = op === '&&' ? intersect : subtract;
      return {
        text: parts.map((p) => p.text).join(op),
        may: op === '&&' ? parts.every((p) => p.may) : parts[0].may,
        ev: (cx) => parts.slice(1).reduce((acc, p) => combine(acc, p.ev(cx)), parts[0].ev(cx)),
        tries: parts.flatMap((p) => p.tries),
        strings: parts.flatMap((p) => p.strings ?? []),
      };
    }
    const parts = [];
    for (let n = 1 + Math.floor(r() * 3); n > 0; n--) {
      if (chance(0.25)) {
        const [lo, hi] = pick(RANGES);
        const a = cps(lo)[0];
        const b = cps(hi)[0];
        parts.push({ text: `${lo}-${hi}`, may: false, ev: (cx) => cx.fold(set((c) => c >= a && c <= b)), tries: [a, b, a + 1, b + 1, a - 1] });
      } else {
        parts.push(operand(depth));
      }
    }
    return {
      text: parts.map((p) => p.text).join(''),
      may: parts.some((p) => p.may),
      ev: (cx) => parts.slice(1).reduce((acc, p) => union(acc, p.ev(cx)), parts[0].ev(cx)),
      tries: parts.flatMap((p) => p.tries),
      strings: parts.flatMap((p) => p.strings ?? []),
    };
  }
  return nested;
}

// CompileAtom :: CharacterClass, anchored at the start of input: the length (code units) of the first alternative
// that matches, or -1. Strings (more than one character) longest first, then the single characters, then the
// empty string; characters compare with Canonicalize.
function expected(value, cx, input) {
  const inp = cps(input);
  const strings = [...value.strings].map(cps).sort((a, b) => b.length - a.length);
  for (const t of strings) {
    if (t.length === 0 || t.length > inp.length) continue;
    if (t.every((c, i) => cx.canon(c) === cx.canon(inp[i]))) return str(inp.slice(0, t.length)).length;
  }
  if (inp.length) {
    const ch = inp[0];
    const found = cx.ignoreCase ? pre(scf(ch)).some((a) => value.has(a)) : value.has(ch);
    if (found) return str([ch]).length;
  }
  return value.strings.has('') ? 0 : -1;
}

function inputsFor(cls, pick) {
  const chars = new Set(cls.tries.filter((c) => c >= 0 && c <= 0x10ffff));
  for (const c of [...chars]) {
    for (const x of pre(scf(c))) chars.add(x);
    const s = String.fromCodePoint(c);
    for (const v of [s.toUpperCase(), s.toLowerCase()]) if (cps(v).length === 1) chars.add(cps(v)[0]);
  }
  for (const s of ['a', 'Z', '5', ' ', 'ſ', 'K', 'ß', 'Ω', '😀', '\ud83d', '-', '&']) chars.add(cps(s)[0]);
  const list = [...chars].map((c) => String.fromCodePoint(c));
  const strings = (cls.strings ?? []).flatMap((t) => [t, t.toUpperCase(), t.toLowerCase(), `${t}${pick(list)}`]);
  const out = new Set(['', ...list, ...strings]);
  for (let i = 0; i < 6; i++) out.add(pick(list) + pick(list));
  return [...out];
}

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(`--${name}`);
  return i < 0 ? dflt : args[i + 1];
};
const r = rng(Number(opt('seed', 1)));
const pick = (a) => a[Math.floor(r() * a.length)];
const nested = generator(r);
const cases = [];
const want = [];
for (let n = Number(opt('classes', 3000)); n > 0; n--) {
  const cls = nested(0);
  for (const ignoreCase of [false, true]) {
    const cx = ctx(ignoreCase);
    const value = cls.ev(cx);
    const flags = ignoreCase ? 'vi' : 'v';
    for (const input of inputsFor(cls, pick)) {
      const len = expected(value, cx, input);
      cases.push({ pattern: `^${cls.text}`, flags, input });
      want.push(len < 0 ? 'N' : `M 0 ${len}`);
      cases.push({ pattern: `^${cls.text}$`, flags, input });
      const full = len === input.length || (cps(input).length > 1 && [...value.strings].some((t) => t.length === input.length && cps(t).every((c, i) => cx.canon(c) === cx.canon(cps(input)[i]))));
      want.push(full || (len === input.length && len >= 0) ? `M 0 ${input.length}` : 'N');
    }
  }
}
const ours = runExecDriver(cases);
const bad = [];
for (let i = 0; i < cases.length; i++) if (ours[i] !== want[i]) bad.push({ ...cases[i], ours: ours[i], spec: want[i] });
console.log(`${cases.length} execs: agree ${cases.length - bad.length}, mismatches ${bad.length}`);
for (const b of bad.slice(0, Number(opt('show', 30)))) {
  console.log(`  /${JSON.stringify(b.pattern).slice(1, -1)}/${b.flags} on ${JSON.stringify(b.input)}: spec ${b.spec}, ours ${b.ours}`);
}
process.exitCode = bad.length ? 1 : 0;
