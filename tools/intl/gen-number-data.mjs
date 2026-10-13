// gen-number-data.mjs: generate src/builtins/intl/number_data.inc, the Intl.NumberFormat locale data, from node's
// own Intl.NumberFormat output (formatToParts of sample values turned into templates).
//   cd tools/intl && npm install && node gen-number-data.mjs
//
// A template is a list of pieces separated by U+001E; each piece is a type letter and its text:
//   N number   C currency (the display string is inserted)   L literal   M minusSign   P plusSign   % percentSign
//   U unit     K compact
// Plural-dependent data lists categories separated by U+001D, each a category letter (z o t f m x = zero one two
// few many other) and its value. Fields of a record are separated by U+001F. Locale data is inherited along the
// BestAvailableLocale truncation chain, so a locale is listed only where it differs from its parent; the root is "".
import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { availableLocales, cmp, dataKey, header, parentOf, u16 } from './common.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const RS = '\x1e', GS = '\x1d', US = '\x1f';
// Data is keyed by dataKey (zh-TW is stored as zh-Hant-TW and inherits from zh-Hant).
const locales = [...new Set(availableLocales().map(dataKey))].sort(cmp);
// Locales with compact, unit and currency-name data (the rest inherit them, ultimately from "en").
const rich = ['ar', 'de', 'de-CH', 'en', 'en-GB', 'en-IN', 'es', 'fa', 'fr', 'hi', 'id', 'it', 'ja', 'ko', 'nl', 'pl', 'pt',
  'pt-PT', 'ru', 'sr', 'sv', 'th', 'tr', 'zh', 'zh-Hant'];
const currencyList = ['AUD', 'BRL', 'CAD', 'CHF', 'CLF', 'CNY', 'EUR', 'GBP', 'IDR', 'INR', 'IQD', 'JPY', 'KMF', 'KRW', 'MXN',
  'PLN', 'RUB', 'SEK', 'THB', 'TWD', 'USD', 'XDR', 'XTS'];
const categoryLetter = { zero: 'z', one: 'o', two: 't', few: 'f', many: 'm', other: 'x' };

const pieceType = { literal: 'L', minusSign: 'M', plusSign: 'P', percentSign: '%', currency: 'C', unit: 'U', compact: 'K' };
const numberTypes = new Set(['integer', 'group', 'decimal', 'fraction', 'nan', 'infinity']);
function template(parts) {
  const out = [];
  for (const p of parts) {
    if (numberTypes.has(p.type)) {
      if (out.at(-1) !== 'N') out.push('N');
      continue;
    }
    const t = pieceType[p.type];
    if (!t) throw new Error(`unexpected part ${p.type}`);
    out.push(t === 'C' ? 'C' : t + p.value);
  }
  return out.join(RS);
}
const parts = (locale, options, x) => new Intl.NumberFormat(locale, options).formatToParts(x);
const signTemplates = (locale, options, x) =>
  [template(parts(locale, options, x)), template(parts(locale, { ...options, signDisplay: 'always' }, x)),
    template(parts(locale, options, -x))].join(US);

// ---- symbols per locale and numbering system ----
function symbols(locale) {
  const value = (options, x, type) => parts(locale, options, x).find((p) => p.type === type)?.value ?? '';
  return [
    value({ minimumFractionDigits: 1 }, 1.5, 'decimal'),
    value({ useGrouping: 'always' }, 1234567, 'group'),
    value({}, -1, 'minusSign'),
    value({ signDisplay: 'always' }, 1, 'plusSign'),
    value({ style: 'percent' }, 1, 'percentSign'),
    value({ notation: 'scientific' }, 12345, 'exponentSeparator'),
    value({ notation: 'scientific' }, 0.00012345, 'exponentMinusSign'),
    value({}, Infinity, 'infinity'),
    value({}, NaN, 'nan'),
    new Intl.NumberFormat(locale).formatRangeToParts(3, 3).filter((p) => p.type === 'approximatelySign').map((p) => p.value).join(''),
  ].join(US);
}

// ---- grouping and the style templates ----
function grouping(locale, useGrouping) {
  const f = new Intl.NumberFormat(locale, { useGrouping });
  const groups = f.formatToParts(123456789012).filter((p) => p.type === 'integer').map((p) => p.value.length);
  const primary = groups.at(-1), secondary = groups.length > 2 ? groups.at(-2) : primary;
  const min = f.format(10 ** primary).includes(f.formatToParts(1234567).find((p) => p.type === 'group')?.value ?? '\0') ? 1 : 2;
  return `${primary}${secondary}${min}`;
}
// The pattern fields, each inherited on its own: grouping (primary, secondary, minimum digits), then the
// zero/plus/minus templates of decimal, percent and currency (symbol, code, accounting symbol, accounting code, name).
const patternFields = ['grouping', 'decimal', 'percent', 'symbol', 'code', 'accountingSymbol', 'accountingCode', 'name', 'range'];
function patternField(locale, field) {
  const cur = (display, sign) => signTemplates(locale, { style: 'currency', currency: 'EUR', currencyDisplay: display, currencySign: sign }, 1);
  switch (field) {
    case 'grouping': return grouping(locale, 'auto');
    case 'decimal': return signTemplates(locale, {}, 1);
    case 'percent': return signTemplates(locale, { style: 'percent' }, 0.01);
    case 'symbol': return cur('symbol', 'standard');
    case 'code': return cur('code', 'standard');
    case 'accountingSymbol': return cur('symbol', 'accounting');
    case 'accountingCode': return cur('code', 'accounting');
    case 'name': return cur('name', 'standard');
    case 'range': {
      const ps = new Intl.NumberFormat(locale).formatRangeToParts(3, 5);
      return ps.filter((p) => p.source === 'shared').map((p) => p.value).join('');
    }
  }
}

// ---- plural-dependent data ----
const categories = (locale) => new Intl.PluralRules(locale).resolvedOptions().pluralCategories;
const baseSamples = [1, 2, 3, 4, 5, 6, 7, 11, 12, 20, 21, 22, 25, 100, 101, 102, 111, 1.5, 0.5, 2.5, 0];
// Samples with 1 to 5 integer digits.
const samples = [...baseSamples];
for (let d = 4; d <= 5; d++) for (const k of [0, 1, 2, 3, 5, 11, 21]) samples.push(10 ** (d - 1) + k), samples.push(2 * 10 ** (d - 1) + k);
// The first sample of each category among numbers with `digits` integer digits (any when digits is null).
function samplesByCategory(locale, digits) {
  const pr = new Intl.PluralRules(locale);
  const out = new Map();
  for (const s of samples) {
    if (digits !== null && String(Math.trunc(s)).length !== digits) continue;
    const c = pr.select(s);
    if (!out.has(c)) out.set(c, s);
  }
  return out;
}
const byCategory = (map) => [...map].sort((a, b) => cmp(categoryLetter[a[0]], categoryLetter[b[0]]))
  .map(([c, v]) => categoryLetter[c] + v).join(GS);

// Compact: for magnitudes 3..14 the exponent ("0" when the magnitude is not compacted) and the templates by category.
function compact(locale, display) {
  const out = [];
  for (let m = 3; m <= 14; m++) {
    const f = new Intl.NumberFormat(locale, { notation: 'compact', compactDisplay: display });
    const ps = f.formatToParts(10 ** m);
    if (!ps.some((p) => p.type === 'compact')) {
      out.push('0');
      continue;
    }
    const digits = ps.filter((p) => p.type === 'integer').map((p) => p.value).join('').length;
    const exponent = m - (digits - 1);
    const t = new Map();
    for (const [c, s] of samplesByCategory(locale, digits)) {
      if (s === 0) continue;
      t.set(c, template(f.formatToParts(s * 10 ** exponent)));
    }
    out.push(`${exponent}${GS}${byCategory(t)}`);
  }
  return out.join(US);
}

const units = [...Intl.supportedValuesOf('unit'), 'kilometer-per-hour'].sort(cmp);
function unit(locale, u) {
  const widths = ['short', 'narrow', 'long'].map((w) => {
    const t = new Map();
    for (const [c, s] of samplesByCategory(locale, null)) t.set(c, template(parts(locale, { style: 'unit', unit: u, unitDisplay: w }, s)));
    // Drop categories equal to "other".
    for (const [c, v] of t) if (c !== 'other' && v === t.get('other')) t.delete(c);
    return byCategory(t);
  });
  if (widths[1] === widths[0]) widths[1] = '';  // narrow as short
  return widths.join(US);
}

function currencyNames(locale, code) {
  const display = (d) => parts(locale, { style: 'currency', currency: code, currencyDisplay: d }, 1).find((p) => p.type === 'currency').value;
  const names = new Map();
  for (const [c, s] of samplesByCategory(locale, null)) {
    names.set(c, parts(locale, { style: 'currency', currency: code, currencyDisplay: 'name' }, s).find((p) => p.type === 'currency').value);
  }
  for (const [c, v] of names) if (c !== 'other' && v === names.get('other')) names.delete(c);
  return [display('symbol'), display('narrowSymbol'), byCategory(names)].join(US);
}

// ---- inheritance ----
function inherited(keys, valueOf, rootValue, keyOf = (l) => l, parentKey = (l) => parentOf(l)) {
  const values = new Map();
  const get = (l) => (l === null ? rootValue : values.has(keyOf(l)) ? values.get(keyOf(l)) : get(parentKey(l)));
  const out = [];
  for (const l of keys) {
    const v = valueOf(l);
    if (v !== get(parentKey(l))) out.push([keyOf(l), v]);
    values.set(keyOf(l), v);
  }
  return out;
}

const defaultNu = (l) => new Intl.NumberFormat(l).resolvedOptions().numberingSystem;
// Symbols: "locale/nu" for latn and each locale's default; "/nu" are the root symbols of every numbering system.
const rootSymbols = [];
for (const nu of Intl.supportedValuesOf('numberingSystem')) {
  if (new Intl.NumberFormat(`en-u-nu-${nu}`).resolvedOptions().numberingSystem !== nu) continue;
  rootSymbols.push([`/${nu}`, symbols(nu === 'latn' ? 'en' : `en-u-nu-${nu}`)]);
}
const symbolEntries = [...rootSymbols];
for (const nu of ['latn', ...new Set(locales.map(defaultNu))].filter((v, i, a) => a.indexOf(v) === i)) {
  const ls = locales.filter((l) => nu === 'latn' || defaultNu(l) === nu || locales.some((c) => c.startsWith(l + '-') && defaultNu(c) === nu));
  const rootValue = rootSymbols.find(([k]) => k === `/${nu}`)[1];
  for (const [k, v] of inherited(ls, (l) => symbols(`${l}-u-nu-${nu}`), rootValue, (l) => `${l}/${nu}`,
    (l) => { let p = parentOf(l); while (p !== null && !ls.includes(p)) p = parentOf(p); return p; })) symbolEntries.push([k, v]);
}
symbolEntries.sort((a, b) => cmp(a[0], b[0]));

const patternEntries = [];
patternFields.forEach((field, i) => {
  const rootValue = patternField('en', field);
  patternEntries.push([`/${i}`, rootValue]);
  for (const [k, v] of inherited(locales, (l) => patternField(l, field), rootValue, (l) => `${l}/${i}`)) patternEntries.push([k, v]);
});
patternEntries.sort((a, b) => cmp(a[0], b[0]));
const richSorted = [...rich].sort(cmp);
const compactEntries = [['', compact('en', 'short') + US + US + compact('en', 'long')]];
for (const l of richSorted) if (l !== 'en') compactEntries.push([l, compact(l, 'short') + US + US + compact(l, 'long')]);
// Unit patterns of the rich locales, inherited along the truncation chain of rich locales (root: en).
const richParent = (l) => { let p = parentOf(l); while (p !== null && !rich.includes(p)) p = parentOf(p); return p === 'en' ? null : p; };
const unitEntries = [];
for (const u of units) {
  const rootValue = unit('en', u);
  unitEntries.push([`/${u}`, rootValue]);
  for (const [k, v] of inherited(richSorted.filter((l) => l !== 'en'), (l) => unit(l, u), rootValue, (l) => `${l}/${u}`, richParent)) unitEntries.push([k, v]);
}
unitEntries.sort((a, b) => cmp(a[0], b[0]));
const currencyEntries = [];
for (const code of Intl.supportedValuesOf('currency')) currencyEntries.push([`/${code}`, currencyNames('en', code)]);
for (const l of richSorted) if (l !== 'en') for (const code of currencyList) currencyEntries.push([`${l}/${code}`, currencyNames(l, code)]);
currencyEntries.sort((a, b) => cmp(a[0], b[0]));
// Currency digits that are not 2.
const currencyDigits = [];
for (const code of Intl.supportedValuesOf('currency')) {
  const d = new Intl.NumberFormat('en', { style: 'currency', currency: code }).resolvedOptions().maximumFractionDigits;
  if (d !== 2) currencyDigits.push([code, String(d)]);
}
for (const code of ['CLF', 'XDR', 'XTS']) {
  const d = new Intl.NumberFormat('en', { style: 'currency', currency: code }).resolvedOptions().maximumFractionDigits;
  if (d !== 2 && !currencyDigits.some(([c]) => c === code)) currencyDigits.push([code, String(d)]);
}
currencyDigits.sort((a, b) => cmp(a[0], b[0]));

const table = (name, entries, comment) => `// ${comment}\nstatic constexpr DataEntry ${name}[] = {\n` +
  entries.map(([k, v]) => `    {"${k}", u"${u16(v)}"},`).join('\n') + '\n};\n';
const out = [
  header('gen-number-data.mjs'),
  '// Format: see the comment at the top of tools/intl/gen-number-data.mjs.',
  '',
  table('kNumberSymbols', symbolEntries, '"locale/nu" -> decimal, group, minus, plus, percent, exponent separator, exponent minus, infinity, nan, approximately.'),
  table('kNumberPatterns', patternEntries, '"locale/field" -> grouping (field 0: primary, secondary, minimum digits) or the zero/plus/minus templates of decimal, percent, currency symbol, code, accounting symbol, accounting code, name (fields 1-7), range separator (field 8).'),
  table('kCompactPatterns', compactEntries, 'locale -> short then long (after an empty field): for magnitudes 3..14, exponent and templates by category.'),
  table('kUnitPatterns', unitEntries, '"locale/unit" -> short, narrow, long templates by category.'),
  table('kCurrencyNames', currencyEntries, '"locale/CODE" -> symbol, narrow symbol, names by category.'),
  'struct CurrencyDigitsEntry {', '  const char* code;', '  int digits;', '};',
  '// ISO 4217 minor units where they are not 2.',
  'static constexpr CurrencyDigitsEntry kCurrencyDigits[] = {',
  ...currencyDigits.map(([c, d]) => `    {"${c}", ${d}},`),
  '};',
  '',
].join('\n');
writeFileSync(path.join(root, 'src/builtins/intl/number_data.inc'), out);
console.log(`number_data.inc: ${symbolEntries.length} symbol, ${patternEntries.length} pattern, ${compactEntries.length} compact, ` +
  `${unitEntries.length} unit, ${currencyEntries.length} currency entries; ${(out.length / 1024).toFixed(0)} KB`);
