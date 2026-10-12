// gen-regexp.mjs: generate the RegExp engine's Unicode tables.
//   cd tools/unicode && npm install && node gen-regexp.mjs
// writes src/unicode/property_tables.inc (\p{...} names and code point sets, properties of strings) and
// src/unicode/case_tables.inc (Canonicalize: simple case folding for u/v, the toUppercase rule otherwise).
//
// Sources: code point sets are node's own (\p{...} with the u flag); the names are ECMA-262 2025 tables 67-69
// (written out below) and PropertyValueAliases.txt for Unicode 17.0 (the npm package mirrors it); the strings of
// the properties of strings and CaseFolding.txt come from @unicode/unicode-17.0.0. Each is cross-checked against
// node, which aborts the generation on any disagreement.
import { writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const require = createRequire(import.meta.url);
const valueAliases = require('unicode-property-value-aliases-ecmascript');
const ucd = (p) => import(`@unicode/unicode-17.0.0/${p}`).then((m) => m.default);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

if (process.versions.unicode !== '17.0') throw new Error(`node has Unicode ${process.versions.unicode}, need 17.0`);
const fail = (msg) => {
  throw new Error(msg);
};

// ECMA-262 2025 table 68: binary properties, canonical name first, then its aliases.
const BINARY = [
  ['ASCII'], ['ASCII_Hex_Digit', 'AHex'], ['Alphabetic', 'Alpha'], ['Any'], ['Assigned'], ['Bidi_Control', 'Bidi_C'],
  ['Bidi_Mirrored', 'Bidi_M'], ['Case_Ignorable', 'CI'], ['Cased'], ['Changes_When_Casefolded', 'CWCF'],
  ['Changes_When_Casemapped', 'CWCM'], ['Changes_When_Lowercased', 'CWL'], ['Changes_When_NFKC_Casefolded', 'CWKCF'],
  ['Changes_When_Titlecased', 'CWT'], ['Changes_When_Uppercased', 'CWU'], ['Dash'], ['Default_Ignorable_Code_Point', 'DI'],
  ['Deprecated', 'Dep'], ['Diacritic', 'Dia'], ['Emoji'], ['Emoji_Component', 'EComp'], ['Emoji_Modifier', 'EMod'],
  ['Emoji_Modifier_Base', 'EBase'], ['Emoji_Presentation', 'EPres'], ['Extended_Pictographic', 'ExtPict'],
  ['Extender', 'Ext'], ['Grapheme_Base', 'Gr_Base'], ['Grapheme_Extend', 'Gr_Ext'], ['Hex_Digit', 'Hex'],
  ['IDS_Binary_Operator', 'IDSB'], ['IDS_Trinary_Operator', 'IDST'], ['ID_Continue', 'IDC'], ['ID_Start', 'IDS'],
  ['Ideographic', 'Ideo'], ['Join_Control', 'Join_C'], ['Logical_Order_Exception', 'LOE'], ['Lowercase', 'Lower'],
  ['Math'], ['Noncharacter_Code_Point', 'NChar'], ['Pattern_Syntax', 'Pat_Syn'], ['Pattern_White_Space', 'Pat_WS'],
  ['Quotation_Mark', 'QMark'], ['Radical'], ['Regional_Indicator', 'RI'], ['Sentence_Terminal', 'STerm'],
  ['Soft_Dotted', 'SD'], ['Terminal_Punctuation', 'Term'], ['Unified_Ideograph', 'UIdeo'], ['Uppercase', 'Upper'],
  ['Variation_Selector', 'VS'], ['White_Space', 'space'], ['XID_Continue', 'XIDC'], ['XID_Start', 'XIDS'],
];
// Table 69: binary properties of strings (v flag only).
const STRINGS = [
  'Basic_Emoji', 'Emoji_Keycap_Sequence', 'RGI_Emoji_Modifier_Sequence', 'RGI_Emoji_Flag_Sequence',
  'RGI_Emoji_Tag_Sequence', 'RGI_Emoji_ZWJ_Sequence', 'RGI_Emoji',
];

// Property values of gc and sc (scx takes sc's values), grouped: canonical (long) name -> all its names.
const valueGroups = (prop) => {
  const groups = new Map();
  for (const [alias, canonical] of valueAliases.get(prop)) {
    if (!groups.has(canonical)) groups.set(canonical, new Set([canonical]));
    groups.get(canonical).add(alias);
  }
  return [...groups].map(([canonical, names]) => ({ canonical, names: [...names] }));
};

// ---- code point sets, from node ----
const allCodePoints = (() => {
  const parts = [];
  for (let cp = 0; cp < 0x110000; cp++) if (cp < 0xd800 || cp > 0xdfff) parts.push(String.fromCodePoint(cp));
  return parts.join('');
})();
const mergeRanges = (rs) => {
  rs.sort((a, b) => a[0] - b[0]);
  const out = [];
  for (const r of rs) {
    const last = out[out.length - 1];
    if (last && last[1] + 1 >= r[0]) last[1] = Math.max(last[1], r[1]);
    else out.push([r[0], r[1]]);
  }
  return out;
};
const nodeRanges = (escape) => {
  const out = [];
  for (const m of allCodePoints.matchAll(new RegExp(`${escape}+`, 'gu'))) {
    const s = m[0];
    const tail = s.charCodeAt(s.length - 1);
    const last = tail >= 0xdc00 && tail <= 0xdfff ? s.codePointAt(s.length - 2) : tail;
    // allCodePoints skips the surrogates, so a run across U+D7FF..U+E000 is two ranges.
    const first = s.codePointAt(0);
    if (first < 0xd800 && last > 0xdfff) out.push([first, 0xd7ff], [0xe000, last]);
    else out.push([first, last]);
  }
  const one = new RegExp(`^${escape}$`, 'u');
  for (let c = 0xd800; c <= 0xdfff; c++) if (one.test(String.fromCharCode(c))) out.push([c, c]);
  return mergeRanges(out);
};
const accepts = (source, flags) => {
  try {
    new RegExp(source, flags);
    return true;
  } catch {
    return false;
  }
};

// Range lists are pooled: identical sets (most sc/scx pairs) share storage.
const pool = [];
const poolIndex = new Map();
const poolRef = (ranges) => {
  const key = ranges.map((r) => `${r[0]}-${r[1]}`).join(',');
  if (!poolIndex.has(key)) {
    poolIndex.set(key, { offset: pool.length, count: ranges.length });
    pool.push(...ranges);
  }
  return poolIndex.get(key);
};

const gcGroups = valueGroups('General_Category');
const scGroups = valueGroups('Script');
const gc = gcGroups.map((g) => poolRef(nodeRanges(`\\p{General_Category=${g.canonical}}`)));
// Katakana_Or_Hiragana is in PropertyValueAliases.txt, so the spec allows it; no code point has it. V8 rejects
// names whose set is empty, so node can't be asked: the set is empty for sc and scx alike.
const scriptSet = (prop, g) =>
  g.canonical === 'Katakana_Or_Hiragana' ? [] : nodeRanges(`\\p{${prop}=${g.canonical}}`);
const sc = scGroups.map((g) => poolRef(scriptSet('Script', g)));
const scx = scGroups.map((g) => poolRef(scriptSet('Script_Extensions', g)));
const bin = BINARY.map(([canonical]) => poolRef(nodeRanges(`\\p{${canonical}}`)));

// node must accept every name we accept (except Katakana_Or_Hiragana, above).
for (const g of gcGroups)
  for (const n of g.names) {
    if (!accepts(`\\p{${n}}`, 'u') || !accepts(`\\p{gc=${n}}`, 'u')) fail(`node rejects gc value ${n}`);
  }
for (const g of scGroups)
  for (const n of g.names) {
    if (g.canonical !== 'Katakana_Or_Hiragana' && !accepts(`\\p{sc=${n}}`, 'u')) fail(`node rejects sc=${n}`);
  }
for (const names of BINARY) for (const n of names) if (!accepts(`\\p{${n}}`, 'u')) fail(`node rejects ${n}`);

// ---- properties of strings ----
const stringProps = [];
for (const name of STRINGS) {
  const strings = await ucd(`Sequence_Property/${name}/index.mjs`);
  const all = new RegExp(`^\\p{${name}}$`, 'v');
  const single = [];
  const multi = [];
  for (const s of strings) {
    if (!all.test(s)) fail(`node: ${name} does not match ${JSON.stringify(s)}`);
    const cps = [...s].map((c) => c.codePointAt(0));
    if (cps.length === 1) single.push([cps[0], cps[0]]);
    else multi.push(cps);
  }
  const ranges = mergeRanges(single);
  multi.sort((a, b) => b.length - a.length || (a.join() < b.join() ? -1 : 1));
  stringProps.push({ name, ranges: poolRef(ranges), multi });
}
// Cross-check the single code points of the properties of strings with node (v mode, code point by code point).
for (const p of stringProps) {
  const re = new RegExp(`^\\p{${p.name}}$`, 'v');
  const mine = new Set();
  for (let i = p.ranges.offset; i < p.ranges.offset + p.ranges.count; i++)
    for (let c = pool[i][0]; c <= pool[i][1]; c++) mine.add(c);
  for (let cp = 0; cp < 0x110000; cp++) {
    if (re.test(String.fromCodePoint(cp)) !== mine.has(cp)) fail(`${p.name}: node disagrees at U+${cp.toString(16)}`);
  }
}

// ---- Canonicalize ----
// u/v: simple case folding (CaseFolding.txt statuses C and S).
const scf = new Map([...(await ucd('Case_Folding/C/code-points.mjs')), ...(await ucd('Case_Folding/S/code-points.mjs'))]);
// otherwise: toUppercase, kept only if it is one code unit and doesn't map non-ASCII to ASCII.
const legacy = new Map();
for (let ch = 0; ch < 0x10000; ch++) {
  const u = String.fromCharCode(ch).toUpperCase();
  if (u.length !== 1) continue;
  const cu = u.charCodeAt(0);
  if (ch >= 128 && cu < 128) continue;
  if (cu !== ch) legacy.set(ch, cu);
}
const canonClasses = (map, limit) => {
  const classes = new Map();
  for (let c = 0; c < limit; c++) {
    const k = map.get(c) ?? c;
    if (!classes.has(k)) classes.set(k, []);
    classes.get(k).push(c);
  }
  return classes;
};
const esc = (c) => `\\u{${c.toString(16)}}`;
const escBmp = (c) => `\\u${c.toString(16).padStart(4, '0')}`;
// Every equivalence class must match exactly in node: each member matches each other member, and no case mapping
// of a member outside the class matches.
const checkClasses = (map, limit, flags, escape, str) => {
  const classes = canonClasses(map, limit);
  const classOf = (c) => classes.get(map.get(c) ?? c);
  for (let c = 0; c < limit; c++) {
    const ch = str(c);
    const candidates = new Set([...classOf(c)]);
    for (const m of [ch.toLowerCase(), ch.toUpperCase()]) if ([...m].length === 1) candidates.add(m.codePointAt(0));
    if (candidates.size === 1 && !map.has(c)) continue;
    const re = new RegExp(`^${escape(c)}$`, flags);
    const members = new Set(classOf(c));
    for (const d of candidates) {
      if (d >= limit) continue;
      if (re.test(str(d)) !== members.has(d)) fail(`/${escape(c)}/${flags} vs U+${d.toString(16)}: node disagrees`);
    }
  }
};
checkClasses(scf, 0x110000, 'iu', esc, (c) => String.fromCodePoint(c));
checkClasses(legacy, 0x10000, 'i', escBmp, (c) => String.fromCharCode(c));

// ---- output ----
const hex = (n) => `0x${n.toString(16)}`;
const rows = (items, per) => {
  const lines = [];
  for (let i = 0; i < items.length; i += per) lines.push(`  ${items.slice(i, i + per).join(', ')},`);
  return lines.join('\n');
};
const nameTable = (cname, entries) => {
  // entries: [name, index]; sorted by byte order for binary search.
  entries.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
  return `static constexpr NameEntry ${cname}[] = {\n${rows(entries.map(([n, i]) => `{"${n}", ${i}}`), 4)}\n};\n`;
};
const refTable = (cname, refs) =>
  `static constexpr RangeRef ${cname}[] = {\n${rows(refs.map((r) => `{${r.offset}, ${r.count}}`), 8)}\n};\n`;

let out = `// Generated by tools/unicode/gen-regexp.mjs from Unicode ${process.versions.unicode} (node ${process.version}). Do not edit.\n`;
out += `// Code point sets for \\p{...}: ranges are pooled in kRanges; each property value is a RangeRef into it.\n\n`;
out += `static constexpr CodePointRange kRanges[] = {\n${rows(pool.map((r) => `{${hex(r[0])}, ${hex(r[1])}}`), 6)}\n};\n\n`;
out += refTable('kGeneralCategoryRanges', gc);
out += nameTable('kGeneralCategoryNames', gcGroups.flatMap((g, i) => g.names.map((n) => [n, i])));
out += refTable('kScriptRanges', sc);
out += refTable('kScriptExtensionsRanges', scx);
out += nameTable('kScriptNames', scGroups.flatMap((g, i) => g.names.map((n) => [n, i])));
out += refTable('kBinaryRanges', bin);
out += nameTable('kBinaryNames', BINARY.flatMap((names, i) => names.map((n) => [n, i])));
// Properties of strings: single code points as ranges, longer strings (longest first) in kStringPool.
const strPool = [];
const strRefs = [];
const strProps = stringProps.map((p) => {
  const first = strRefs.length;
  for (const cps of p.multi) {
    strRefs.push([strPool.length, cps.length]);
    strPool.push(...cps);
  }
  return { ranges: p.ranges, first, strings: p.multi.length };
});
out += `\nstatic constexpr char32_t kStringPool[] = {\n${rows(strPool.map(hex), 10)}\n};\n`;
out += `static constexpr StringRef kStrings[] = {\n${rows(strRefs.map(([o, l]) => `{${o}, ${l}}`), 8)}\n};\n`;
out += `static constexpr StringProperty kStringProperties[] = {\n${rows(
  strProps.map((p) => `{{${p.ranges.offset}, ${p.ranges.count}}, ${p.first}, ${p.strings}}`),
  1,
)}\n};\n`;
out += nameTable('kStringPropertyNames', STRINGS.map((n, i) => [n, i]));
writeFileSync(path.join(root, 'src/unicode/property_tables.inc'), out);

const pairs = (map) => [...map].sort((a, b) => a[0] - b[0]).map(([f, t]) => `{${hex(f)}, ${hex(t)}}`);
let cases = `// Generated by tools/unicode/gen-regexp.mjs from Unicode ${process.versions.unicode} (node ${process.version}). Do not edit.\n`;
cases += `// Canonicalize (ECMA-262 22.2.2.7.3). Code points not listed map to themselves.\n\n`;
cases += `// u or v flag: simple case folding (CaseFolding.txt, statuses C and S).\n`;
cases += `static constexpr CaseMapping kSimpleCaseFolding[] = {\n${rows(pairs(scf), 6)}\n};\n\n`;
cases += `// neither: toUppercase when it is a single code unit, not mapping a non-ASCII unit to ASCII.\n`;
cases += `static constexpr CaseMapping kLegacyCanonicalize[] = {\n${rows(pairs(legacy), 6)}\n};\n`;
writeFileSync(path.join(root, 'src/unicode/case_tables.inc'), cases);
console.log(
  `ranges ${pool.length}, gc ${gcGroups.length}, sc ${scGroups.length}, binary ${BINARY.length}, ` +
    `strings ${strRefs.length} (${strPool.length} code points), scf ${scf.size}, legacy ${legacy.size}`,
);
