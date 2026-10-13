// gen-plural-data.mjs: generate src/builtins/intl/plural_data.inc, the CLDR plural rules (cardinal and ordinal,
// without their samples) and plural ranges for Intl.PluralRules.
//   cd tools/intl && npm install && node gen-plural-data.mjs
// Each rules entry lists the categories other than "other" in CLDR order, separated by U+001D: the category letter
// (z o t f m = zero one two few many) followed by its condition. Each ranges entry lists "<start><end><result>"
// triples of category letters (x = other).
import { writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { cmp, header } from './common.mjs';

const require = createRequire(import.meta.url);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const supp = (name) => require(`cldr-core/supplemental/${name}.json`).supplemental;
const letter = { zero: 'z', one: 'o', two: 't', few: 'f', many: 'm', other: 'x' };
const order = ['zero', 'one', 'two', 'few', 'many'];

function rules(table) {
  return Object.entries(table).map(([locale, r]) => {
    const parts = [];
    for (const c of order) {
      const rule = r[`pluralRule-count-${c}`];
      if (rule === undefined) continue;
      const condition = rule.split('@')[0].trim();
      parts.push(letter[c] + condition);
    }
    return [locale, parts.join('\x1d')];
  }).sort((a, b) => cmp(a[0], b[0]));
}
const cardinal = rules(supp('plurals')['plurals-type-cardinal']);
const ordinal = rules(supp('ordinals')['plurals-type-ordinal']);
const ranges = Object.entries(supp('pluralRanges').plurals).map(([locale, r]) => {
  const triples = Object.entries(r).map(([k, v]) => {
    const [, start, end] = k.match(/^pluralRange-start-(\w+)-end-(\w+)$/);
    return letter[start] + letter[end] + letter[v];
  }).sort(cmp);
  return [locale, triples.join('')];
}).sort((a, b) => cmp(a[0], b[0]));

const table = (name, entries, comment) => `// ${comment}\nstatic constexpr AliasEntry ${name}[] = {\n` +
  entries.map(([k, v]) => `    {"${k}", "${v.replace(/\x1d/g, '\\035')}"},`).join('\n') + '\n};\n';
const out = [
  header('gen-plural-data.mjs'),
  '',
  table('kCardinalRules', cardinal, 'plurals-type-cardinal: locale -> conditions by category.'),
  table('kOrdinalRules', ordinal, 'plurals-type-ordinal: locale -> conditions by category.'),
  table('kPluralRanges', ranges, 'pluralRanges: locale -> start/end/result category triples.'),
].join('\n');
writeFileSync(path.join(root, 'src/builtins/intl/plural_data.inc'), out);
console.log(`plural_data.inc: ${cardinal.length} cardinal, ${ordinal.length} ordinal, ${ranges.length} range entries; ${(out.length / 1024).toFixed(0)} KB`);
