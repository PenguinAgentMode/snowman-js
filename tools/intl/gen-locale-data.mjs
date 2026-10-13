// gen-locale-data.mjs: generate src/builtins/intl/locale_data.inc, the per-locale data of the Intl services from
// node's own Intl (ICU): the available locales, default numbering system / calendar / hour cycle, collations,
// numbering system digits.
//   cd tools/intl && npm install && node gen-locale-data.mjs
// Per-locale defaults are stored only where they differ from the parent's (the parent is the locale with its last
// subtag removed, as BestAvailableLocale truncates); the root value is the table's documented default.
import { writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { cmp, dataKey, header } from './common.mjs';

const require = createRequire(import.meta.url);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const likely = require('cldr-core/supplemental/likelySubtags.json').supplemental.likelySubtags;

// ---- available locales: every language[-script][-region] combination node resolves to itself ----
const langs = new Set(), scripts = new Set(), regions = new Set();
for (const v of Object.values(likely)) {
  const [l, s, r] = v.split('-');
  langs.add(l);
  scripts.add(s);
  regions.add(r);
}
const exact = (t) => new Intl.NumberFormat(t).resolvedOptions().locale === t;
const locales = [];
for (const l of [...langs].sort(cmp)) {
  if (!exact(l)) continue;
  locales.push(l);
  for (const r of [...regions].sort(cmp)) if (exact(`${l}-${r}`)) locales.push(`${l}-${r}`);
  for (const s of [...scripts].sort(cmp)) {
    if (!exact(`${l}-${s}`)) continue;
    locales.push(`${l}-${s}`);
    for (const r of [...regions].sort(cmp)) if (exact(`${l}-${s}-${r}`)) locales.push(`${l}-${s}-${r}`);
  }
}
if (!locales.includes('en-US')) throw new Error('en-US must be available');
locales.sort(cmp);

const parentOf = (locale) => {
  let pos = locale.lastIndexOf('-');
  if (pos < 0) return null;
  if (pos >= 2 && locale[pos - 2] === '-') pos -= 2;
  return locale.slice(0, pos);
};
// The entries of `valueOf` that differ from the inherited value (rootValue at the top), keyed by dataKey.
const dataLocales = [...new Set(locales.map(dataKey))].sort(cmp);
function inherited(valueOf, rootValue) {
  const value = new Map();
  const get = (l) => (l === null ? rootValue : value.has(l) ? value.get(l) : get(parentOf(l)));
  const out = [];
  for (const l of dataLocales) {
    const v = valueOf(l);
    if (v !== get(parentOf(l))) out.push([l, v]);
    value.set(l, v);
  }
  return out;
}

const defaultNu = inherited((l) => new Intl.NumberFormat(l).resolvedOptions().numberingSystem, 'latn');
const defaultCa = inherited((l) => new Intl.DateTimeFormat(l).resolvedOptions().calendar, 'gregory');
const defaultHc = inherited((l) => new Intl.DateTimeFormat(l, { hour: 'numeric' }).resolvedOptions().hourCycle, 'h23');
// A collation is supported when the resolved locale keeps its -u-co- keyword (ICU reports a locale's own default,
// e.g. zh pinyin, as "default" in resolvedOptions().collation).
const collations = inherited((l) => Intl.supportedValuesOf('collation')
  .filter((c) => new Intl.Collator(`${l}-u-co-${c}`).resolvedOptions().locale.endsWith(`-u-co-${c}`)).join(' '), 'emoji eor');

// ---- numbering systems with simple digit mappings ----
const digits = [];
for (const nu of Intl.supportedValuesOf('numberingSystem')) {
  const f = new Intl.NumberFormat('en', { numberingSystem: nu, useGrouping: false });
  if (f.resolvedOptions().numberingSystem !== nu) continue;
  const ds = [...'0123456789'].map((d) => f.format(Number(d)));
  if (ds.some((d) => [...d].length !== 1)) continue;
  digits.push([nu, ds.join('')]);
}

// ---- supportedValuesOf lists ----
const listOf = (key) => [...Intl.supportedValuesOf(key)].sort(cmp);
// The Intl era/monthCode calendars (node also lists "islamic" and "islamic-rgsa", which ECMA-402 no longer has).
const calendars = listOf('calendar').filter((c) => c !== 'islamic' && c !== 'islamic-rgsa');
const collationTypes = listOf('collation');
const currencies = listOf('currency');
const units = listOf('unit');

// ---- time zones (cldr-bcp47): each non-deprecated bcp47 zone is one primary identifier (its IANA name, else the
// first alias) and its other names are links to it; Etc/UTC and Etc/GMT and their links are all "UTC" (ECMA-402).
const bcp47Zones = require('cldr-bcp47/bcp47/timezone.json').keyword.u.tz;
const zoneNames = new Map();  // lowercase name -> primary
const zoneRegions = [];  // [region, primary]
for (const [id, info] of Object.entries(bcp47Zones)) {
  if (id.startsWith('_') || info._deprecated || !info._alias || id === 'unk') continue;
  const names = info._alias.split(' ');
  const primary = id === 'utc' || id === 'gmt' ? 'UTC' : (info._iana ?? names[0]);
  for (const n of [...names, primary]) {
    const key = n.toLowerCase();
    if (zoneNames.has(key) && zoneNames.get(key) !== primary) throw new Error(`time zone ${n}: two primaries`);
    zoneNames.set(key, primary);
  }
  if (id.length === 5) zoneRegions.push([id.slice(0, 2).toUpperCase(), primary]);
}
const timeZones = [...zoneNames].sort((a, b) => cmp(a[0], b[0]));
zoneRegions.sort((a, b) => cmp(a[0], b[0]) || cmp(a[1], b[1]));

// ---- Locale info (cldr-core): calendar preferences, hour cycles and week data per region; RTL scripts ----
const supp = (name) => require(`cldr-core/supplemental/${name}.json`).supplemental;
const bcp47Calendar = { gregorian: 'gregory', 'ethiopic-amete-alem': 'ethioaa' };
const regionCalendars = Object.entries(supp('calendarPreferenceData').calendarPreferenceData)
  .map(([r, list]) => [r, list.map((c) => bcp47Calendar[c] ?? c).join(' ')]).sort((a, b) => cmp(a[0], b[0]));
const hourSymbol = { h: 'h12', hb: 'h12', hB: 'h12', H: 'h23', Hb: 'h23', HB: 'h23', K: 'h11', k: 'h24' };
const regionHourCycles = Object.entries(supp('timeData').timeData).map(([r, t]) => {
  const list = [hourSymbol[t._preferred]];
  for (const a of t._allowed.split(' ')) if (!list.includes(hourSymbol[a])) list.push(hourSymbol[a]);
  return [r.replace(/^.*_/, ''), list.join(' '), r];
}).filter(([, , key]) => !key.includes('_') || key.startsWith('und_')).map(([r, l]) => [r, l]).sort((a, b) => cmp(a[0], b[0]));
const week = supp('weekData').weekData;
const dayNumber = { mon: 1, tue: 2, wed: 3, thu: 4, fri: 5, sat: 6, sun: 7 };
const weekRegions = new Set([...Object.keys(week.firstDay), ...Object.keys(week.minDays), ...Object.keys(week.weekendStart),
  ...Object.keys(week.weekendEnd)].filter((r) => !r.includes('-')));
const weekOf = (table, r) => table[r] ?? table['001'];
const regionWeeks = [...weekRegions].sort(cmp).map((r) =>
  [r, `${dayNumber[weekOf(week.firstDay, r)]}${weekOf(week.minDays, r)}${dayNumber[weekOf(week.weekendStart, r)]}${dayNumber[weekOf(week.weekendEnd, r)]}`]);
const rtlScripts = [...scripts].filter((sc) => new Intl.Locale(`und-${sc}`).textInfo.direction === 'rtl').sort(cmp);
const ltrScripts = [...scripts].filter((sc) => !sc.startsWith('Z') && new Intl.Locale(`und-${sc}`).textInfo.direction === 'ltr').sort(cmp);

const stringList = (name, list, comment) =>
  `// ${comment}\nstatic constexpr const char* ${name}[] = {\n` +
  Array.from({ length: Math.ceil(list.length / 8) }, (_, i) => '    ' + list.slice(i * 8, i * 8 + 8).map((l) => `"${l}",`).join(' ')).join('\n') +
  '\n};\n';

const esc = (s) => [...s].map((c) => (c.charCodeAt(0) < 0x80 ? c : [...c].map((u) => `\\u${u.charCodeAt(0).toString(16).padStart(4, '0')}`).join(''))).join('');
const u16 = (s) => {
  let out = '';
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    out += c < 0x80 && c !== 0x22 && c !== 0x5c ? s[i] : `\\x${c.toString(16)}` + (i + 1 < s.length && /[0-9a-fA-F]/.test(s[i + 1]) ? '""' : '');
  }
  return out;
};
void esc;
const pairs = (name, list, comment) =>
  `// ${comment}\nstatic constexpr AliasEntry ${name}[] = {\n` + list.map(([a, b]) => `    {"${a}", "${b}"},`).join('\n') + '\n};\n';

const out = [
  header('gen-locale-data.mjs'),
  '',
  '// The available locales (canonical, sorted): every service supports all of them.',
  'static constexpr const char* kLocales[] = {',
  ...Array.from({ length: Math.ceil(locales.length / 10) }, (_, i) =>
    '    ' + locales.slice(i * 10, i * 10 + 10).map((l) => `"${l}",`).join(' ')),
  '};',
  '',
  pairs('kDefaultNumberingSystems', defaultNu, 'Default numbering system where it differs from the parent locale\'s (root: latn).'),
  pairs('kDefaultCalendars', defaultCa, 'Default calendar where it differs from the parent locale\'s (root: gregory).'),
  pairs('kDefaultHourCycles', defaultHc, 'Default hour cycle where it differs from the parent locale\'s (root: h23).'),
  pairs('kCollations', collations, 'Collations besides "default" where they differ from the parent locale\'s (root: "emoji eor").'),
  stringList('kCalendars', calendars, 'Intl.supportedValuesOf("calendar").'),
  stringList('kCollationTypes', collationTypes, 'Intl.supportedValuesOf("collation").'),
  stringList('kCurrencies', currencies, 'Intl.supportedValuesOf("currency").'),
  stringList('kUnits', units, 'Intl.supportedValuesOf("unit"): the sanctioned simple units.'),
  pairs('kTimeZones', timeZones, 'Time zone names (lowercase) -> primary identifier.'),
  pairs('kTimeZoneRegions', zoneRegions, 'Region -> primary time zone identifiers in it, sorted.'),
  pairs('kRegionCalendars', regionCalendars, 'calendarPreferenceData: region -> calendars by preference.'),
  pairs('kRegionHourCycles', regionHourCycles, 'timeData: region -> hour cycles, preferred first.'),
  pairs('kRegionWeeks', regionWeeks, 'weekData: region -> firstDay, minDays, weekendStart, weekendEnd (1 = Monday).'),
  stringList('kRtlScripts', rtlScripts, 'Scripts written right to left.'),
  stringList('kLtrScripts', ltrScripts, 'Scripts written left to right (others, like Zyyy, have no direction).'),
  '// Numbering systems with a simple digit mapping: the UTF-16 digits 0-9.',
  'struct DigitsEntry {',
  '  const char* name;',
  '  const char16_t* digits;',
  '};',
  'static constexpr DigitsEntry kNumberingSystemDigits[] = {',
  ...digits.map(([n, d]) => `    {"${n}", u"${u16(d)}"},`),
  '};',
  '',
].join('\n');
writeFileSync(path.join(root, 'src/builtins/intl/locale_data.inc'), out);
console.log(`locale_data.inc: ${locales.length} locales; ${defaultNu.length} nu, ${defaultCa.length} ca, ${defaultHc.length} hc, ` +
  `${collations.length} co entries; ${digits.length} numbering systems; ${timeZones.length} time zone names`);
