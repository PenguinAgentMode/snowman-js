// gen-relative-data.mjs: generate src/builtins/intl/relative_data.inc, the Intl.RelativeTimeFormat patterns, from
// node's own Intl.RelativeTimeFormat.
//   cd tools/intl && npm install && node gen-relative-data.mjs
// An entry "locale/unit/style" holds, separated by U+001F: the future patterns and the past patterns by plural
// category (U+001D-separated, each the category letter z o t f m x, the text before the number, U+001E, the text
// after it, or U+0002 and the whole text for a pattern without the number), then the relative names of numeric "auto" (U+001D-separated "value=name"). Locales without data inherit
// along the dataKey truncation chain of the listed locales, ultimately from en.
import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { cmp, header, u16 } from './common.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const locales = ['ar', 'de', 'en', 'en-GB', 'es', 'fr', 'hi', 'id', 'it', 'ja', 'ko', 'nl', 'pl', 'pt', 'pt-PT', 'ru', 'sr', 'sv',
  'th', 'tr', 'zh', 'zh-Hant'];
const units = ['second', 'minute', 'hour', 'day', 'week', 'month', 'quarter', 'year'];
const letter = { zero: 'z', one: 'o', two: 't', few: 'f', many: 'm', other: 'x' };
const samples = [1, 2, 3, 4, 5, 6, 7, 11, 12, 20, 21, 22, 25, 100, 101, 102, 111, 1.5, 0.5, 2.5, 0, 1000000];

function entry(locale, unit, style) {
  const rtf = new Intl.RelativeTimeFormat(locale, { style, numeric: 'always' });
  const auto = new Intl.RelativeTimeFormat(locale, { style, numeric: 'auto' });
  const pr = new Intl.PluralRules(locale);
  const patterns = (sign) => {
    const byCategory = new Map();
    for (const s of samples) {
      const c = pr.select(s);
      if (byCategory.has(c)) continue;
      const parts = rtf.formatToParts(sign * s, unit);
      const first = parts.findIndex((p) => p.type !== 'literal');
      if (first < 0) {
        // A pattern without the number (ar "one": "قبل ثانية واحدة"): U+0002 then the text.
        byCategory.set(c, `\x02${parts.map((p) => p.value).join('')}`);
        continue;
      }
      let last = parts.length - 1;
      while (parts[last].type === 'literal') last--;
      const before = parts.slice(0, first).map((p) => p.value).join('');
      const after = parts.slice(last + 1).map((p) => p.value).join('');
      byCategory.set(c, `${before}\x1e${after}`);
    }
    return [...byCategory].sort((a, b) => cmp(letter[a[0]], letter[b[0]])).map(([c, v]) => letter[c] + v).join('\x1d');
  };
  const names = [];
  for (const v of [-2, -1, 0, 1, 2]) {
    const parts = auto.formatToParts(v, unit);
    if (parts.length === 1 && parts[0].type === 'literal') names.push(`${v}=${parts[0].value}`);
  }
  return [patterns(1), patterns(-1), names.join('\x1d')].join('\x1f');
}

const entries = [];
for (const l of locales) for (const u of units) for (const s of ['long', 'short', 'narrow']) entries.push([`${l === 'en' ? '' : l}/${u}/${s}`, entry(l, u, s)]);
entries.sort((a, b) => cmp(a[0], b[0]));
const out = [
  header('gen-relative-data.mjs'),
  '',
  '// "locale/unit/style" -> future patterns, past patterns, relative names (see the generator).',
  'static constexpr DataEntry kRelativePatterns[] = {',
  ...entries.map(([k, v]) => `    {"${k}", u"${u16(v)}"},`),
  '};',
  '',
].join('\n');
writeFileSync(path.join(root, 'src/builtins/intl/relative_data.inc'), out);
console.log(`relative_data.inc: ${entries.length} entries; ${(out.length / 1024).toFixed(0)} KB`);
