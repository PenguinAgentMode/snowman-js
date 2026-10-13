// gen-date-data.mjs: generate src/builtins/intl/date_data.inc, the Intl.DateTimeFormat data of the Gregorian
// calendar: names, date/time patterns, availableFormats skeletons, time zone formats (cldr-dates-full), day
// period rules and metazones (cldr-core).
//   cd tools/intl && npm install && node gen-date-data.mjs
// An entry "locale/field" holds one field (list items separated by U+001E); fields are inherited along the
// dataKey truncation chain of the listed locales (root: en). Fields:
//   0-5 months: format abbreviated/narrow/wide, stand-alone abbreviated/narrow/wide (January first)
//   6-13 days: format abbreviated/narrow/short/wide, stand-alone same (Sunday first)
//   14-16 eras: abbreviated, wide, narrow (BC, AD)
//   17-19 day periods (format abbreviated/narrow/wide): am pm midnight noon morning1 morning2 afternoon1 afternoon2
//         evening1 evening2 night1 night2 ("" where absent)
//   20 dateFormats, 21 timeFormats, 22 dateTimeFormats, 23 dateTimeFormats-atTime (full long medium short)
//   24 availableFormats ("skeleton=pattern"), 25 appendItems ("Key=pattern")
//   26 time zone formats: hourFormat, gmtFormat, gmtZeroFormat
//   27 day period rules ("name=from-before" in minutes, "name=@at")
//   28 Etc/UTC names: long, short
//   29 intervalFormats: the fallback, then "skeleton/greatestDifference=pattern"
//   30 field display names (appendItems {2}): era year quarter month week weekday day dayperiod hour minute second zone
// In patterns U+202F (before day periods) becomes U+0020, the web-compatibility substitution V8 and SpiderMonkey make.
//
// The other calendars (cldr-cal-*-full; "generic" from cldr-dates-full) are kCalendarFields, "calendar:locale/field"
// with the same field numbers (0-13, 17-25, 29; months in CLDR order: hebrew has 1-13, then 7-yeartype-leap), plus:
//   31 monthPatterns leap: format abbreviated/narrow/wide, stand-alone abbreviated/narrow/wide, numeric
//   32-34 cyclic year names (cyclicNameSets years format): abbreviated, narrow, wide (jia-zi first)
//   35-37 eras by Temporal era code ("code=name"): abbreviated, wide, narrow
// A field is looked up locale-first: for each locale of the chain (root last), the calendar's chain (calendarChain:
// islamic-civil -> islamic -> generic), then that locale's Gregorian field. An entry is written only where that
// lookup would otherwise give a different value (datetimeformat.cpp calendarField does the same lookup).
import { writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { cmp, header, parentOf, u16 } from './common.mjs';

const require = createRequire(import.meta.url);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const RS = '\x1e';
const rich = ['ar', 'de', 'en', 'en-AU', 'en-CA', 'en-GB', 'en-IN', 'es', 'es-419', 'fa', 'fr', 'fr-CA', 'he', 'hi', 'id', 'it', 'ja',
  'ko', 'nl', 'pl', 'pt', 'pt-PT', 'ru', 'sr', 'sv', 'th', 'tr', 'uk', 'zh', 'zh-Hant'].sort(cmp);
const cldrName = (l) => l;  // cldr-json directory names match these locale ids

const periods = ['am', 'pm', 'midnight', 'noon', 'morning1', 'morning2', 'afternoon1', 'afternoon2', 'evening1', 'evening2', 'night1',
  'night2'];
const dayRules = require('cldr-core/supplemental/dayPeriods.json').supplemental.dayPeriodRuleSet;
const minutes = (hhmm) => { const [h, m] = hhmm.split(':').map(Number); return h * 60 + m; };

// The fields every calendar has (0-13, 17-25, 29) of a CLDR calendar object; the others are left unset.
function calendarFields(g) {
  const out = [];
  // Months by CLDR key: 1, 2, ... then the non-numeric ones (hebrew 7-yeartype-leap).
  const monthKeys = Object.keys(g.months.format.wide).sort((a, b) => (Number(a) || 1e9) - (Number(b) || 1e9) || cmp(a, b));
  const months = (ctx, w) => monthKeys.map((k) => g.months[ctx][w][k] ?? '').join(RS);
  for (const ctx of ['format', 'stand-alone']) for (const w of ['abbreviated', 'narrow', 'wide']) out.push(months(ctx, w));
  const dayKeys = ['sun', 'mon', 'tue', 'wed', 'thu', 'fri', 'sat'];
  for (const ctx of ['format', 'stand-alone']) for (const w of ['abbreviated', 'narrow', 'short', 'wide']) out.push(dayKeys.map((k) => g.days[ctx][w][k]).join(RS));
  out.length = 17;
  for (const w of ['abbreviated', 'narrow', 'wide']) out.push(periods.map((p) => g.dayPeriods.format[w][p] ?? '').join(RS));
  const four = (o) => ['full', 'long', 'medium', 'short'].map((s) => (typeof o[s] === 'string' ? o[s] : o[s]._value)).join(RS);
  out.push(four(g.dateFormats), four(g.timeFormats), four(g.dateTimeFormats), four(g['dateTimeFormats-atTime'].standard));
  const available = Object.entries(g.dateTimeFormats.availableFormats).filter(([k]) => !k.includes('-alt-') && !k.includes('-count-'))
    .map(([k, v]) => `${k}=${v}`).sort(cmp);
  out.push(available.join(RS));
  out.push(Object.entries(g.dateTimeFormats.appendItems).map(([k, v]) => `${k}=${v}`).sort(cmp).join(RS));
  const intervals = g.dateTimeFormats.intervalFormats;
  const items = [];
  for (const [skeleton, diffs] of Object.entries(intervals)) {
    if (skeleton === 'intervalFormatFallback' || skeleton.includes('-alt-')) continue;
    for (const [d, pattern] of Object.entries(diffs)) if (!d.includes('-alt-')) items.push(`${skeleton}/${d}=${pattern}`);
  }
  out[29] = [intervals.intervalFormatFallback, ...items.sort(cmp)].join(RS);
  for (const i of [20, 21, 22, 23, 24, 25, 29]) out[i] = out[i].replace(/\u202f/g, ' ');
  return out;
}

function fields(locale) {
  const g = require(`cldr-dates-full/main/${cldrName(locale)}/ca-gregorian.json`).main[cldrName(locale)].dates.calendars.gregorian;
  const tz = require(`cldr-dates-full/main/${cldrName(locale)}/timeZoneNames.json`).main[cldrName(locale)].dates.timeZoneNames;
  const out = calendarFields(g);
  for (const [i, w] of ['eraAbbr', 'eraNames', 'eraNarrow'].entries()) out[14 + i] = [g.eras[w]['0'], g.eras[w]['1']].join(RS);
  out[26] = [tz.hourFormat, tz.gmtFormat, tz.gmtZeroFormat].join(RS);
  const language = locale.split('-')[0];
  const rules = dayRules[language] ?? dayRules.en;
  out[27] = Object.entries(rules).map(([k, v]) => (v._at ? `${k}=@${minutes(v._at)}` : `${k}=${minutes(v._from)}-${minutes(v._before)}`)).sort(cmp).join(RS);
  const utc = tz.zone?.Etc?.UTC ?? require('cldr-dates-full/main/en/timeZoneNames.json').main.en.dates.timeZoneNames.zone.Etc.UTC;
  out[28] = [utc.long?.standard ?? '', utc.short?.standard ?? ''].join(RS);
  const df = require(`cldr-dates-full/main/${cldrName(locale)}/dateFields.json`).main[cldrName(locale)].dates.fields;
  out[30] = ['era', 'year', 'quarter', 'month', 'week', 'weekday', 'day', 'dayperiod', 'hour', 'minute', 'second', 'zone']
    .map((f) => df[f]?.displayName ?? '').join(RS);
  return out;
}

// The other calendars: BCP 47 id, npm package, CLDR calendar name, and the CLDR era index of each Temporal era code
// (japanese bce/ce use the Gregorian eras). "generic" and "islamic" are only inherited from (calendarChain).
const calendars = [
  ['generic', 'cldr-dates-full', 'generic', {}],
  ['buddhist', 'cldr-cal-buddhist-full', 'buddhist', { be: 0 }],
  ['chinese', 'cldr-cal-chinese-full', 'chinese', {}],
  ['coptic', 'cldr-cal-coptic-full', 'coptic', { am: 1 }],
  ['dangi', 'cldr-cal-dangi-full', 'dangi', {}],
  ['ethiopic', 'cldr-cal-ethiopic-full', 'ethiopic', { aa: 0, am: 1 }],
  ['ethioaa', 'cldr-cal-ethiopic-full', 'ethiopic-amete-alem', { aa: 0 }],
  ['hebrew', 'cldr-cal-hebrew-full', 'hebrew', { am: 0 }],
  ['indian', 'cldr-cal-indian-full', 'indian', { shaka: 0 }],
  ['islamic', 'cldr-cal-islamic-full', 'islamic', { ah: 0, bh: 1 }],
  ['islamic-civil', 'cldr-cal-islamic-full', 'islamic-civil', { ah: 0, bh: 1 }],
  ['islamic-tbla', 'cldr-cal-islamic-full', 'islamic-tbla', { ah: 0, bh: 1 }],
  ['islamic-umalqura', 'cldr-cal-islamic-full', 'islamic-umalqura', { ah: 0, bh: 1 }],
  ['japanese', 'cldr-cal-japanese-full', 'japanese', { meiji: 232, taisho: 233, showa: 234, heisei: 235, reiwa: 236 }],
  ['persian', 'cldr-cal-persian-full', 'persian', { ap: 0 }],
  ['roc', 'cldr-cal-roc-full', 'roc', { broc: 0, roc: 1 }],
];
const calendarChain = (c) => {
  if (c === 'generic' || c === 'chinese') return [c];
  if (c === 'dangi') return [c, 'chinese'];
  if (c.startsWith('islamic-')) return [c, 'islamic', 'generic'];
  if (c === 'ethioaa') return [c, 'ethiopic', 'generic'];
  return [c, 'generic'];
};

function otherCalendarFields(locale, [, pkg, name, eraCodes]) {
  const g = require(`${pkg}/main/${cldrName(locale)}/ca-${name}.json`).main[cldrName(locale)].dates.calendars[name];
  const out = calendarFields(g);
  if (g.monthPatterns) {
    const leap = (ctx, w) => g.monthPatterns[ctx]?.[w]?.leap ?? '';
    out[31] = [leap('format', 'abbreviated'), leap('format', 'narrow'), leap('format', 'wide'), leap('stand-alone', 'abbreviated'),
      leap('stand-alone', 'narrow'), leap('stand-alone', 'wide'), leap('numeric', 'all')].join(RS);
  }
  const years = g.cyclicNameSets?.years?.format;
  if (years) for (const [i, w] of ['abbreviated', 'narrow', 'wide'].entries()) out[32 + i] = Array.from({ length: 60 }, (_, k) => years[w][String(k + 1)]).join(RS);
  const codes = Object.entries(eraCodes).sort((a, b) => cmp(a[0], b[0]));
  if (codes.length) for (const [i, w] of ['eraAbbr', 'eraNames', 'eraNarrow'].entries()) out[35 + i] = codes.map(([code, k]) => `${code}=${g.eras[w][String(k)] ?? ''}`).join(RS);
  return out;
}

const values = new Map(rich.map((l) => [l, fields(l)]));
const richParent = (l) => { let p = parentOf(l); while (p !== null && !rich.includes(p)) p = parentOf(p); return p; };
const entries = [];
const rootFields = values.get('en');
rootFields.forEach((v, i) => entries.push([`/${i}`, v]));
for (const l of rich) {
  if (l === 'en') continue;
  values.get(l).forEach((v, i) => {
    let p = richParent(l);
    const inherited = p === null ? rootFields[i] : values.get(p)[i];
    if (v !== inherited) entries.push([`${l}/${i}`, v]);
  });
}
entries.sort((a, b) => cmp(a[0], b[0]));

// The other calendars, with calendarField's lookup simulated: root (en) first, then each locale after its parents.
const gregorian = new Map(entries);
const calendarEntries = new Map();
const keyOf = (l) => (l === 'en' ? '' : l);
const chainOf = (l) => { const out = []; for (let c = l; c !== null; c = parentOf(c)) out.push(keyOf(c)); if (out.at(-1) !== '') out.push(''); return out; };
const lookup = (l, cal, i) => {
  for (const x of chainOf(l)) {
    for (const c of calendarChain(cal)) if (calendarEntries.has(`${c}:${x}/${i}`)) return calendarEntries.get(`${c}:${x}/${i}`);
    if (gregorian.has(`${x}/${i}`)) return gregorian.get(`${x}/${i}`);
  }
  return '';
};
const depth = (l) => l.split('-').length;
const byParents = [...rich].sort((a, b) => (a === 'en' ? -1 : b === 'en' ? 1 : depth(a) - depth(b) || cmp(a, b)));
for (const cal of calendars) {
  for (const l of byParents) {
    otherCalendarFields(l, cal).forEach((v, i) => {
      // The generic calendar's names are month codes (M01): only its patterns are inherited.
      if (cal[0] === 'generic' && !(i >= 20 && i <= 25) && i !== 29) return;
      if (v !== undefined && lookup(l, cal[0], i) !== v) calendarEntries.set(`${cal[0]}:${keyOf(l)}/${i}`, v);
    });
  }
}
const calendarList = [...calendarEntries].sort((a, b) => cmp(a[0], b[0]));

// English time zone names by metazone, and the current metazone of each zone.
const enTz = require('cldr-dates-full/main/en/timeZoneNames.json').main.en.dates.timeZoneNames;
const metaNames = Object.entries(enTz.metazone).map(([m, n]) =>
  [m, [n.long?.generic, n.long?.standard, n.long?.daylight, n.short?.generic, n.short?.standard, n.short?.daylight].map((s) => s ?? '').join(RS)])
  .sort((a, b) => cmp(a[0], b[0]));
const zoneMeta = [];
const walk = (o, prefix) => {
  for (const [k, v] of Object.entries(o)) {
    if (Array.isArray(v)) {
      const current = v.find((u) => !u.usesMetazone._to) ?? v.at(-1);
      zoneMeta.push([prefix + k, current.usesMetazone._mzone]);
    } else walk(v, `${prefix}${k}/`);
  }
};
walk(require('cldr-core/supplemental/metaZones.json').supplemental.metaZones.metazoneInfo.timezone, '');
// Every name of a bcp47 zone (Asia/Kolkata as well as CLDR's Asia/Calcutta) gets the zone's metazone.
const metaOf = new Map(zoneMeta);
for (const [id, info] of Object.entries(require('cldr-bcp47/bcp47/timezone.json').keyword.u.tz)) {
  if (id.startsWith('_') || !info._alias) continue;
  const names = [...info._alias.split(' '), ...(info._iana ? [info._iana] : [])];
  const meta = names.map((n) => metaOf.get(n)).find(Boolean);
  if (meta) for (const n of names) if (!metaOf.has(n)) zoneMeta.push([n, meta]), metaOf.set(n, meta);
}
zoneMeta.sort((a, b) => cmp(a[0], b[0]));

const out = [
  header('gen-date-data.mjs', ['cldr-core', 'cldr-bcp47', 'cldr-dates-full', ...[...new Set(calendars.map((c) => c[1]))].filter((p) => p !== 'cldr-dates-full')]),
  '',
  '// "locale/field" -> field (see the generator for the fields).',
  'static constexpr DataEntry kDateFields[] = {',
  ...entries.map(([k, v]) => `    {"${k}", u"${u16(v)}"},`),
  '};',
  '// "calendar:locale/field" -> field of the other calendars (see the generator for the fields and the lookup).',
  'static constexpr DataEntry kCalendarFields[] = {',
  ...calendarList.map(([k, v]) => `    {"${k}", u"${u16(v)}"},`),
  '};',
  '// English metazone names: long generic, standard, daylight, short generic, standard, daylight.',
  'static constexpr DataEntry kMetazoneNames[] = {',
  ...metaNames.map(([k, v]) => `    {"${k}", u"${u16(v)}"},`),
  '};',
  '// Zone -> its current metazone.',
  'static constexpr AliasEntry kZoneMetazones[] = {',
  ...zoneMeta.map(([k, v]) => `    {"${k}", "${v}"},`),
  '};',
  '',
].join('\n');
writeFileSync(path.join(root, 'src/builtins/intl/date_data.inc'), out);
console.log(`date_data.inc: ${entries.length} fields, ${calendarList.length} calendar fields, ${metaNames.length} metazones, ${zoneMeta.length} zones; ${(out.length / 1024).toFixed(0)} KB`);
