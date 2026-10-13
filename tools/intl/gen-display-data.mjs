// gen-display-data.mjs: generate src/builtins/intl/display_data.inc, the Intl.DisplayNames names, from node's own
// Intl.DisplayNames. English only: other locales fall back to it.
//   cd tools/intl && npm install && node gen-display-data.mjs
// An entry "type/style/code" holds the name; the short and narrow styles are listed only where they differ from
// long. Language names cover the languages of the likely-subtags data, and the dialect names (languageDisplay
// "dialect") of the available locales whose name is not the standard composition.
import { writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { availableLocales, cmp, header, u16 } from './common.mjs';

const require = createRequire(import.meta.url);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const likely = require('cldr-core/supplemental/likelySubtags.json').supplemental.likelySubtags;
const locale = 'en';
const styles = ['long', 'short', 'narrow'];

const codes = { language: new Set(), region: new Set(), script: new Set() };
for (const [k, v] of Object.entries(likely)) {
  for (const t of [k, v]) {
    const l = new Intl.Locale(t);
    if (l.language !== 'und') codes.language.add(l.language);
    if (l.script) codes.script.add(l.script);
    if (l.region) codes.region.add(l.region);
  }
}
for (let i = 0; i < 26 * 26; i++) codes.region.add(String.fromCharCode(65 + i / 26 | 0, 65 + i % 26));
for (const n of ['001', '002', '003', '005', '009', '011', '013', '014', '015', '017', '018', '019', '021', '029', '030', '034', '035',
  '039', '053', '054', '057', '061', '142', '143', '145', '150', '151', '154', '155', '202', '419']) codes.region.add(n);
codes.currency = new Set(Intl.supportedValuesOf('currency'));
codes.calendar = new Set(Intl.supportedValuesOf('calendar'));
codes.dateTimeField = new Set(['era', 'year', 'quarter', 'month', 'weekOfYear', 'weekday', 'day', 'dayPeriod', 'hour', 'minute',
  'second', 'timeZoneName']);

const entries = [];
for (const [type, set] of Object.entries(codes)) {
  for (const code of [...set].sort(cmp)) {
    const names = styles.map((style) => {
      try {
        return new Intl.DisplayNames(locale, { type, style, fallback: 'none', languageDisplay: 'standard' }).of(code);
      } catch {
        return undefined;  // a code node's ICU rejects
      }
    });
    if (names[0] === undefined) continue;
    entries.push([`${type}/long/${code}`, names[0]]);
    if (names[1] !== names[0]) entries.push([`${type}/short/${code}`, names[1]]);
    if (names[2] !== names[1]) entries.push([`${type}/narrow/${code}`, names[2]]);
  }
}
// Dialect names: whole-locale names that are not "language (region)".
for (const style of styles) {
  const dialect = new Intl.DisplayNames(locale, { type: 'language', style, languageDisplay: 'dialect' });
  const standard = new Intl.DisplayNames(locale, { type: 'language', style, languageDisplay: 'standard' });
  for (const l of availableLocales()) {
    if (!l.includes('-')) continue;
    const name = dialect.of(l);
    if (name !== standard.of(l)) entries.push([`dialect/${style}/${l}`, name]);
  }
}
entries.sort((a, b) => cmp(a[0], b[0]));
// The composition patterns.
const dn = new Intl.DisplayNames(locale, { type: 'language', languageDisplay: 'standard' });
const pattern = dn.of('en-GB').replace('English', '{0}').replace('United Kingdom', '{1}');
const separator = dn.of('en-Latn-GB').slice('English ('.length).replace('Latin', '').replace('United Kingdom', '').replace(')', '');
const out = [
  header('gen-display-data.mjs'),
  '',
  `// The locale pattern and the separator between the qualifiers of a language name ("${pattern}", "${separator}").`,
  `static constexpr const char16_t* kLocalePattern = u"${u16(pattern)}";`,
  `static constexpr const char16_t* kLocaleSeparator = u"${u16(separator)}";`,
  '// "type/style/code" -> name (type "dialect" for whole-locale names).',
  'static constexpr DataEntry kDisplayNames[] = {',
  ...entries.map(([k, v]) => `    {"${k}", u"${u16(v)}"},`),
  '};',
  '',
].join('\n');
writeFileSync(path.join(root, 'src/builtins/intl/display_data.inc'), out);
console.log(`display_data.inc: ${entries.length} entries; ${(out.length / 1024).toFixed(0)} KB`);
