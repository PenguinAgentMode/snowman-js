// gen-list-data.mjs: generate src/builtins/intl/list_data.inc, the Intl.ListFormat patterns, from node's own
// Intl.ListFormat (the separators formatToParts puts between the elements of 2- and 4-element lists).
//   cd tools/intl && npm install && node gen-list-data.mjs
// An entry "locale/type/style" holds, separated by U+001F, the literals before, between and after a 2-element list,
// then before, between the first two (start), the middle ones, the last two (end) and after a longer list; entries are
// inherited along the dataKey truncation chain (see common.mjs) and listed only where they differ.
import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { availableLocales, cmp, dataKey, header, parentOf, u16 } from './common.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const locales = [...new Set(availableLocales().map(dataKey))].sort(cmp);

function separators(locale, type, style) {
  const lf = new Intl.ListFormat(locale, { type, style });
  // The literals before, between and after the elements (empty strings where there are none).
  const literals = (list) => {
    const out = [''];
    for (const p of lf.formatToParts(list)) {
      if (p.type === 'element') out.push('');
      else out[out.length - 1] += p.value;
    }
    return out;
  };
  const [before2, pair, after2] = literals(['a', 'b']);
  const [before, start, middle, end, after] = literals(['a', 'b', 'c', 'd']);
  return [before2, pair, after2, before, start, middle, end, after].join('\x1f');
}

const entries = [];
for (const type of ['conjunction', 'disjunction', 'unit']) {
  for (const style of ['long', 'short', 'narrow']) {
    const rootValue = separators('en', type, style);
    entries.push([`/${type}/${style}`, rootValue]);
    const values = new Map();
    const get = (l) => (l === null ? rootValue : values.has(l) ? values.get(l) : get(parentOf(l)));
    for (const l of locales) {
      const v = separators(l, type, style);
      if (v !== get(parentOf(l))) entries.push([`${l}/${type}/${style}`, v]);
      values.set(l, v);
    }
  }
}
entries.sort((a, b) => cmp(a[0], b[0]));
const values = [...new Set(entries.map(([, v]) => v))].sort(cmp);
const out = [
  header('gen-list-data.mjs'),
  '',
  '// The distinct pattern values: 2 elements: before, pair, after; more: before, start, middle, end, after.',
  'static constexpr const char16_t* kListValues[] = {',
  ...values.map((v) => `    u"${u16(v)}",`),
  '};',
  '// "locale/type/style" -> index in kListValues.',
  'struct ListEntry {',
  '  const char* key;',
  '  uint16_t value;',
  '};',
  'static constexpr ListEntry kListPatterns[] = {',
  ...entries.map(([k, v]) => `    {"${k}", ${values.indexOf(v)}},`),
  '};',
  '',
].join('\n');
writeFileSync(path.join(root, 'src/builtins/intl/list_data.inc'), out);
console.log(`list_data.inc: ${entries.length} entries; ${(out.length / 1024).toFixed(0)} KB`);
