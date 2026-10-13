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

function fields(locale) {
  const g = require(`cldr-dates-full/main/${cldrName(locale)}/ca-gregorian.json`).main[cldrName(locale)].dates.calendars.gregorian;
  const tz = require(`cldr-dates-full/main/${cldrName(locale)}/timeZoneNames.json`).main[cldrName(locale)].dates.timeZoneNames;
  const out = [];
  const months = (ctx, w) => Array.from({ length: 12 }, (_, i) => g.months[ctx][w][String(i + 1)]).join(RS);
  for (const ctx of ['format', 'stand-alone']) for (const w of ['abbreviated', 'narrow', 'wide']) out.push(months(ctx, w));
  const dayKeys = ['sun', 'mon', 'tue', 'wed', 'thu', 'fri', 'sat'];
  for (const ctx of ['format', 'stand-alone']) for (const w of ['abbreviated', 'narrow', 'short', 'wide']) out.push(dayKeys.map((k) => g.days[ctx][w][k]).join(RS));
  for (const w of ['eraAbbr', 'eraNames', 'eraNarrow']) out.push([g.eras[w]['0'], g.eras[w]['1']].join(RS));
  for (const w of ['abbreviated', 'narrow', 'wide']) out.push(periods.map((p) => g.dayPeriods.format[w][p] ?? '').join(RS));
  const four = (o) => ['full', 'long', 'medium', 'short'].map((s) => (typeof o[s] === 'string' ? o[s] : o[s]._value)).join(RS);
  out.push(four(g.dateFormats), four(g.timeFormats), four(g.dateTimeFormats), four(g['dateTimeFormats-atTime'].standard));
  const available = Object.entries(g.dateTimeFormats.availableFormats).filter(([k]) => !k.includes('-alt-') && !k.includes('-count-'))
    .map(([k, v]) => `${k}=${v}`).sort(cmp);
  out.push(available.join(RS));
  out.push(Object.entries(g.dateTimeFormats.appendItems).map(([k, v]) => `${k}=${v}`).sort(cmp).join(RS));
  out.push([tz.hourFormat, tz.gmtFormat, tz.gmtZeroFormat].join(RS));
  const language = locale.split('-')[0];
  const rules = dayRules[language] ?? dayRules.en;
  out.push(Object.entries(rules).map(([k, v]) => (v._at ? `${k}=@${minutes(v._at)}` : `${k}=${minutes(v._from)}-${minutes(v._before)}`)).sort(cmp).join(RS));
  const utc = tz.zone?.Etc?.UTC ?? require('cldr-dates-full/main/en/timeZoneNames.json').main.en.dates.timeZoneNames.zone.Etc.UTC;
  out.push([utc.long?.standard ?? '', utc.short?.standard ?? ''].join(RS));
  const intervals = g.dateTimeFormats.intervalFormats;
  const items = [];
  for (const [skeleton, diffs] of Object.entries(intervals)) {
    if (skeleton === 'intervalFormatFallback' || skeleton.includes('-alt-')) continue;
    for (const [d, pattern] of Object.entries(diffs)) if (!d.includes('-alt-')) items.push(`${skeleton}/${d}=${pattern}`);
  }
  out.push([intervals.intervalFormatFallback, ...items.sort(cmp)].join(RS));
  const df = require(`cldr-dates-full/main/${cldrName(locale)}/dateFields.json`).main[cldrName(locale)].dates.fields;
  out.push(['era', 'year', 'quarter', 'month', 'week', 'weekday', 'day', 'dayperiod', 'hour', 'minute', 'second', 'zone']
    .map((f) => df[f]?.displayName ?? '').join(RS));
  for (const i of [20, 21, 22, 23, 24, 25, 29]) out[i] = out[i].replace(/\u202f/g, ' ');
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
zoneMeta.sort((a, b) => cmp(a[0], b[0]));

const out = [
  header('gen-date-data.mjs', ['cldr-core', 'cldr-dates-full']),
  '',
  '// "locale/field" -> field (see the generator for the fields).',
  'static constexpr DataEntry kDateFields[] = {',
  ...entries.map(([k, v]) => `    {"${k}", u"${u16(v)}"},`),
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
console.log(`date_data.inc: ${entries.length} fields, ${metaNames.length} metazones, ${zoneMeta.length} zones; ${(out.length / 1024).toFixed(0)} KB`);
