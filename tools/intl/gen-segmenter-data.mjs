// gen-segmenter-data.mjs: generate src/builtins/intl/segmenter_data.inc, the UAX #29 break properties
// (Grapheme_Cluster_Break, Word_Break, Sentence_Break, Extended_Pictographic) for Intl.Segmenter.
//   cd tools/intl && npm install && node gen-segmenter-data.mjs
// Source: @unicode/unicode-17.0.0 (the Unicode version of node 22, process.versions.unicode). Each table is a
// sorted list of disjoint {first, last, value} ranges; code points not listed are Other (value 0).
import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { header } from './common.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
if (process.versions.unicode !== '17.0') throw new Error(`node has Unicode ${process.versions.unicode}, the data is 17.0`);
const load = async (p) => (await import(`@unicode/unicode-17.0.0/${p}/code-points.mjs`)).default;

const properties = {
  kGraphemeBreak: ['Grapheme_Cluster_Break', ['CR', 'LF', 'Control', 'Extend', 'ZWJ', 'Regional_Indicator', 'Prepend', 'SpacingMark',
    'L', 'V', 'T', 'LV', 'LVT']],
  kWordBreak: ['Word_Break', ['CR', 'LF', 'Newline', 'Extend', 'ZWJ', 'Regional_Indicator', 'Format', 'Katakana', 'Hebrew_Letter',
    'ALetter', 'Single_Quote', 'Double_Quote', 'MidNumLet', 'MidLetter', 'MidNum', 'Numeric', 'ExtendNumLet', 'WSegSpace']],
  kSentenceBreak: ['Sentence_Break', ['CR', 'LF', 'Extend', 'Sep', 'Format', 'Sp', 'Lower', 'Upper', 'OLetter', 'Numeric', 'ATerm',
    'SContinue', 'STerm', 'Close']],
};

function ranges(valueOf) {
  const out = [];
  for (let cp = 0; cp <= 0x10ffff; cp++) {
    const v = valueOf.get(cp) ?? 0;
    if (v === 0) continue;
    const last = out.at(-1);
    if (last && last[1] === cp - 1 && last[2] === v) last[1] = cp;
    else out.push([cp, cp, v]);
  }
  return out;
}

const lines = [header('gen-segmenter-data.mjs', ['@unicode/unicode-17.0.0']), ''];
for (const [name, [property, values]] of Object.entries(properties)) {
  const valueOf = new Map();
  for (const [i, v] of values.entries()) for (const cp of await load(`${property}/${v}`)) valueOf.set(cp, i + 1);
  const rs = ranges(valueOf);
  lines.push(`// ${property}: 0 Other, ${values.map((v, i) => `${i + 1} ${v}`).join(', ')}.`);
  lines.push(`static constexpr BreakRange ${name}[] = {`);
  for (let i = 0; i < rs.length; i += 6) lines.push('    ' + rs.slice(i, i + 6).map(([a, b, v]) => `{0x${a.toString(16)}, 0x${b.toString(16)}, ${v}}`).join(', ') + ',');
  lines.push('};', '');
}
const pict = new Map();
for (const cp of await load('Binary_Property/Extended_Pictographic')) pict.set(cp, 1);
const pr = ranges(pict);
lines.push('// Extended_Pictographic.', 'static constexpr BreakRange kExtendedPictographic[] = {');
for (let i = 0; i < pr.length; i += 6) lines.push('    ' + pr.slice(i, i + 6).map(([a, b, v]) => `{0x${a.toString(16)}, 0x${b.toString(16)}, ${v}}`).join(', ') + ',');
lines.push('};', '');
const out = lines.join('\n');
writeFileSync(path.join(root, 'src/builtins/intl/segmenter_data.inc'), out);
console.log(`segmenter_data.inc: ${(out.length / 1024).toFixed(0)} KB`);
