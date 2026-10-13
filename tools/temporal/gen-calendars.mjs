// gen-calendars.mjs: the month tables of the table-driven Temporal calendars (chinese, dangi, islamic-umalqura),
// generated from node's Intl.DateTimeFormat (ICU) and, for the chinese and dangi years 1900..2100, from astronomy
// (astro.mjs). Output is sorted and has no timestamps, so a re-run with the same node/ICU writes byte-identical files.
//
//   node tools/temporal/gen-calendars.mjs [--out src/builtins/temporal]
//
// Writes calendar_chinese_data.inc, calendar_dangi_data.inc and calendar_islamic_umalqura_data.inc, and prints to
// stderr every astronomical year that differs from ICU and every new moon or major solar term near a midnight that
// decides a month.
//
// Lunisolar years (chinese, dangi): the arithmetic year is ICU's related ISO year. Per year one packed uint32:
//   bits 0..12   month i (0-based ordinal) has 30 days (else 29)
//   bits 13..16  the 1-based ordinal of the leap month (0: no leap month); a leap month after month N is "M0NL"
// plus the epoch day (days since 1970-01-01) of the first year's month 1 day 1; each later year starts where the
// previous one ends.
// ICU computes these calendars with its own low-precision astronomy, which puts a new moon or a major solar term on
// the wrong side of midnight in a few years (chinese new year 2027 and 2030, the 1987 leap month). In ASTRONOMICAL's
// years the months come from astro.mjs instead (Meeus; new moons within ~20 s), with the official rules: a month
// starts on the local day of a new moon; the month containing the December solstice is month 11; in a sui of 13
// months (month 11 to the next month 11) the first month without a major solar term is the leap month.
// Islamic-umalqura: per AH year one uint32 with bit i = month i+1 has 30 days; ICU uses its table for AH 1300..1600
// and the islamic-civil arithmetic outside it.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { newMoon, solarLongitudeTime, utOfJde } from './astro.mjs';

const args = process.argv.slice(2);
const outDir = args.includes('--out') ? args[args.indexOf('--out') + 1] : 'src/builtins/temporal';

const LUNISOLAR_FIRST = -6000;
const LUNISOLAR_LAST = 2200;
const UMALQURA_FIRST = 1300;
const UMALQURA_LAST = 1600;
const MS_PER_DAY = 86400000;
const JD_UNIX_EPOCH = 2440587.5;
// A new moon or major solar term this close to a local midnight is reported as an edge case.
const EDGE_MINUTES = 5;

// The astronomical related ISO years and each calendar's civil time zone (hours east of UT, by UT Julian day).
// Chinese: Beijing local mean time (116°25'E, UTC+7:45:40) before 1929, UTC+8 (120°E) from 1929, as in Dershowitz &
// Reingold, "Calendrical Calculations" (ICU uses UTC+8 throughout; test262 has no year before 1929). Dangi: UTC+8
// until 1911, UTC+9 from 1912 (ICU's rule after KASI; UTC+8:30 in 1954-03-21..1961-08-09 decides no day, and
// 1908-1911 kept UTC+8).
const JD_1912 = 2419402.5 - 9 / 24; // 1912-01-01 00:00 UTC+9
const JD_1929 = 2425612.5 - 8 / 24; // 1929-01-01 00:00 UTC+8
export const ASTRONOMICAL = {
  chinese: {
    first: 1900, last: 2100, zone: (ut) => (ut < JD_1929 ? 1397 / 180 : 8), zoneText: 'UTC+7:45:40 to 1928, UTC+8 from 1929',
  },
  dangi: { first: 1900, last: 2100, zone: (ut) => (ut < JD_1912 ? 8 : 9), zoneText: 'UTC+8 to 1911, UTC+9 from 1912' },
};

function formatter(calendar) {
  const f = new Intl.DateTimeFormat(`en-u-ca-${calendar}`, { timeZone: 'UTC', year: 'numeric', month: 'numeric', day: 'numeric' });
  return (epochDay) => {
    const parts = {};
    for (const p of f.formatToParts(new Date(epochDay * MS_PER_DAY))) parts[p.type] = p.value;
    return { year: Number(parts.relatedYear ?? parts.year), month: parts.month, day: Number(parts.day) };
  };
}

function epochDayOfIso(year, month, day) {
  const d = new Date(0);
  d.setUTCFullYear(year, month - 1, day);
  return Math.round(d.getTime() / MS_PER_DAY);
}

// Walk month starts from `start` (a day 1) while keep(parts) holds: [{ start, length, year, month }].
function walkMonths(at, start, keep) {
  const months = [];
  let s = start;
  for (;;) {
    const here = at(s);
    if (here.day !== 1) throw new Error(`not a month start: ${s}`);
    if (!keep(here)) return months;
    let length = 29;
    if (at(s + 29).day !== 1) {
      length = 30;
      if (at(s + 30).day !== 1) throw new Error(`month at ${s} is neither 29 nor 30 days`);
    }
    months.push({ start: s, length, year: here.year, month: here.month });
    s += length;
  }
}

function findMonthOne(at, from, year) {
  for (let d = from; d < from + 120; d++) {
    const p = at(d);
    if (p.year === year && p.month === '1' && p.day === 1) return d;
  }
  throw new Error(`no month 1 of ${year} after ${from}`);
}

function lunisolar(calendar) {
  const at = formatter(calendar);
  const first = findMonthOne(at, epochDayOfIso(LUNISOLAR_FIRST, 1, 1), LUNISOLAR_FIRST);
  const months = walkMonths(at, first, (p) => p.year <= LUNISOLAR_LAST);
  const years = new Map();
  for (const m of months) {
    if (!years.has(m.year)) years.set(m.year, []);
    years.get(m.year).push(m);
  }
  const packed = [];
  for (let y = LUNISOLAR_FIRST; y <= LUNISOLAR_LAST; y++) {
    const ms = years.get(y);
    if (!ms || (ms.length !== 12 && ms.length !== 13)) throw new Error(`${calendar} ${y}: ${ms?.length} months`);
    let bits = 0;
    let leap = 0;
    let expected = 1;
    ms.forEach((m, i) => {
      if (m.length === 30) bits |= 1 << i;
      if (m.month.endsWith('bis')) {
        if (leap || Number(m.month.slice(0, -3)) !== expected - 1) throw new Error(`${calendar} ${y}: bad leap month ${m.month}`);
        leap = i + 1;
      } else {
        if (Number(m.month) !== expected) throw new Error(`${calendar} ${y}: month ${m.month}, expected ${expected}`);
        expected++;
      }
    });
    if ((ms.length === 13) !== (leap !== 0)) throw new Error(`${calendar} ${y}: 13 months iff a leap month`);
    packed.push(((bits | (leap << 13)) >>> 0));
  }
  return { first, packed };
}

const jdOfIso = (year, month, day) => epochDayOfIso(year, month, day) + JD_UNIX_EPOCH;

// The months of related ISO years first..last computed astronomically in the time zone zone(utJd) (hours), as
// { starts, packed, edges }: starts[i] = epoch day of month 1 day 1 of year first+i (one more entry: the end of
// `last`), packed as lunisolar() packs it, edges = the deciding events within EDGE_MINUTES of a local midnight.
export function astronomical(zone, first, last) {
  const local = (jde) => { const ut = utOfJde(jde); return ut + zone(ut) / 24 - JD_UNIX_EPOCH; };
  const show = (t) => new Date(Math.round(t * MS_PER_DAY)).toISOString().slice(0, 19).replace('T', ' ');
  const nearMidnight = (t) => { const f = t - Math.floor(t); return Math.min(f, 1 - f) * 1440 < EDGE_MINUTES; };
  // New moons from before the December solstice of first-1 to after month 1 of last+1.
  const moons = [];
  for (let k = Math.floor((first - 2001) * 12.3685); ; k++) {
    const t = local(newMoon(k));
    if (t < epochDayOfIso(first - 1, 11, 1)) continue;
    moons.push(t);
    if (t > epochDayOfIso(last + 1, 12, 31)) break;
  }
  const starts = moons.map(Math.floor);
  // Major solar terms (zhongqi): the Sun's apparent longitude a multiple of 30 degrees; 270 is the December solstice.
  const terms = [];
  for (let y = first - 1; y <= last + 1; y++) {
    for (let j = 0; j < 12; j++) {
      const degrees = 30 * j;
      // The Sun is near 280 degrees on January 1.
      const guess = jdOfIso(y, 1, 1) + (((degrees - 280) % 360 + 360) % 360) * 365.2422 / 360;
      terms.push({ degrees, t: local(solarLongitudeTime(degrees, guess)) });
    }
  }
  terms.sort((a, b) => a.t - b.t);
  const monthOf = (day) => { let i = 0; while (starts[i + 1] <= day) i++; return i; };
  const hasTerm = new Set(terms.map((x) => monthOf(Math.floor(x.t))));
  const edges = [];
  for (let i = 0; i < moons.length; i++) if (nearMidnight(moons[i])) edges.push(`new moon ${show(moons[i])}`);
  for (const x of terms) {
    const day = Math.floor(x.t);
    // Only a term on the first or last day of a month can change months by moving across midnight.
    if (nearMidnight(x.t) && (starts.includes(day) || starts.includes(day + 1))) edges.push(`term ${x.degrees} ${show(x.t)}`);
  }
  // Number the months sui by sui, from the month 11 that contains each December solstice.
  const solstice11 = (y) => monthOf(Math.floor(terms.find((x) => x.degrees === 270 && x.t > epochDayOfIso(y, 12, 1)).t));
  const months = [];
  for (let y = first - 1; y <= last; y++) {
    const a = solstice11(y), b = solstice11(y + 1);
    if (b - a !== 12 && b - a !== 13) throw new Error(`astronomical sui ${y}: ${b - a} months`);
    let number = 11, leapDone = b - a === 12;
    for (let i = a; i < b; i++) {
      let leap = false;
      if (i > a) {
        if (!leapDone && !hasTerm.has(i)) leap = leapDone = true; else number = number % 12 + 1;
      }
      months.push({ start: starts[i], length: starts[i + 1] - starts[i], number, leap });
    }
  }
  const yearStarts = months.map((m, i) => (m.number === 1 && !m.leap ? i : -1)).filter((i) => i >= 0);
  if (yearStarts.length !== last - first + 2) throw new Error(`astronomical: ${yearStarts.length} month 1s`);
  const packed = [];
  for (let n = 0; n <= last - first; n++) {
    const ms = months.slice(yearStarts[n], yearStarts[n + 1]);
    let bits = 0, leap = 0;
    ms.forEach((m, i) => {
      if (m.length === 30) bits |= 1 << i; else if (m.length !== 29) throw new Error(`astronomical: ${m.length}-day month`);
      if (m.leap) leap = i + 1;
    });
    const isoYear = new Date(months[yearStarts[n]].start * MS_PER_DAY).getUTCFullYear();
    if (isoYear !== first + n) throw new Error(`astronomical: year ${first + n} starts in ${isoYear}`);
    packed.push((bits | (leap << 13)) >>> 0);
  }
  return { starts: yearStarts.map((i) => months[i].start), packed, edges };
}

// "1987: leap M06L (ICU M07L), 384 days (ICU 384), M06L starts 1987-07-26 (ICU M07 1987-07-26) ..." for a year that
// differs: the leap month, the length, and every month whose first day differs.
function describeYear(year, start, packed, icuStart, icuPacked) {
  const iso = (d) => new Date(d * MS_PER_DAY).toISOString().slice(0, 10);
  const leapText = (p) => { const l = (p >> 13) & 15; return l ? `M${String(l - 1).padStart(2, '0')}L` : 'none'; };
  const months = (s, p) => {
    const leap = (p >> 13) & 15, out = [];
    for (let i = 0; i < (leap ? 13 : 12); i++) {
      const n = leap && i + 1 >= leap ? i : i + 1;
      out.push({ code: `M${String(n).padStart(2, '0')}${leap === i + 1 ? 'L' : ''}`, start: s });
      s += p & (1 << i) ? 30 : 29;
    }
    return { out, end: s };
  };
  const a = months(start, packed), b = months(icuStart, icuPacked);
  const parts = [];
  if (((packed ^ icuPacked) >> 13) & 15) parts.push(`leap ${leapText(packed)} (ICU ${leapText(icuPacked)})`);
  parts.push(`${a.end - start} days (ICU ${b.end - icuStart})`);
  a.out.forEach((m, i) => {
    const o = b.out[i];
    if (m.start === o.start && m.code === o.code) return;
    parts.push(`${m.code} ${iso(m.start)} (ICU ${o.code === m.code ? '' : o.code + ' '}${iso(o.start)})`);
  });
  if (a.end !== b.end) parts.push(`next year ${iso(a.end)} (ICU ${iso(b.end)})`);
  return `${year}: ${parts.join(', ')}`;
}

// ICU's table with ASTRONOMICAL[calendar]'s years replaced by astronomy; the replaced years must start and end on
// ICU's days (the table chains years). Returns lunisolar()'s shape plus the report lines.
export function lunisolarTable(calendar) {
  const icu = lunisolar(calendar);
  const { first, last, zone } = ASTRONOMICAL[calendar];
  const astro = astronomical(zone, first, last);
  const icuStarts = [icu.first];
  for (const p of icu.packed) {
    let n = 0;
    for (let i = 0; i < ((p >> 13) & 15 ? 13 : 12); i++) n += p & (1 << i) ? 30 : 29;
    icuStarts.push(icuStarts[icuStarts.length - 1] + n);
  }
  const at = (y) => y - LUNISOLAR_FIRST;
  if (astro.starts[0] !== icuStarts[at(first)] || astro.starts[last - first + 1] !== icuStarts[at(last + 1)]) {
    throw new Error(`${calendar}: astronomical ${first}..${last} doesn't start and end on ICU's days`);
  }
  const differ = [];
  for (let y = first; y <= last; y++) {
    const n = y - first;
    if (astro.packed[n] !== icu.packed[at(y)] || astro.starts[n] !== icuStarts[at(y)]) {
      differ.push(describeYear(y, astro.starts[n], astro.packed[n], icuStarts[at(y)], icu.packed[at(y)]));
    }
  }
  const packed = icu.packed.slice();
  packed.splice(at(first), last - first + 1, ...astro.packed);
  return { first: icu.first, packed, differ, edges: astro.edges };
}

function umalqura() {
  const at = formatter('islamic-umalqura');
  // 1 Muharram 1300 AH is in November 1882.
  let first = null;
  for (let d = epochDayOfIso(1882, 10, 1); d < epochDayOfIso(1883, 1, 1); d++) {
    const p = at(d);
    if (p.year === UMALQURA_FIRST && p.month === '1' && p.day === 1) { first = d; break; }
  }
  if (first === null) throw new Error('no 1 Muharram 1300');
  const months = walkMonths(at, first, (p) => p.year <= UMALQURA_LAST);
  const packed = [];
  for (let y = UMALQURA_FIRST; y <= UMALQURA_LAST; y++) {
    const ms = months.filter((m) => m.year === y);
    if (ms.length !== 12) throw new Error(`umalqura ${y}: ${ms.length} months`);
    let bits = 0;
    ms.forEach((m, i) => {
      if (Number(m.month) !== i + 1) throw new Error(`umalqura ${y}: month ${m.month}`);
      if (m.length === 30) bits |= 1 << i;
    });
    packed.push(bits);
  }
  return { first, packed };
}

function header(name, what, source = 'node\'s Intl.DateTimeFormat') {
  const v = process.versions;
  return [
    `// ${name}: ${what}`,
    ...`// Generated by tools/temporal/gen-calendars.mjs from ${source}; do not edit.`.split('\n'),
    '// Command: node tools/temporal/gen-calendars.mjs --out src/builtins/temporal',
    `// node ${v.node}, ICU ${v.icu}, CLDR ${v.cldr}, Unicode ${v.unicode}`,
    '',
  ].join('\n');
}

function rows(values, perLine, width) {
  const lines = [];
  for (let i = 0; i < values.length; i += perLine) {
    lines.push('    ' + values.slice(i, i + perLine).map((v) => '0x' + v.toString(16).padStart(width, '0') + ',').join(' '));
  }
  return lines.join('\n');
}

function writeLunisolar(calendar, prefix) {
  const { first, packed, differ, edges } = lunisolarTable(calendar);
  const a = ASTRONOMICAL[calendar];
  const source = `node's Intl.DateTimeFormat (ICU) and, for related ISO years\n// ${a.first}..${a.last}, ` +
    `from tools/temporal/astro.mjs (Meeus; ${a.zoneText})`;
  const text = header(`calendar_${calendar}_data.inc`, `${calendar} month table, related ISO years ${LUNISOLAR_FIRST}..${LUNISOLAR_LAST}`, source) +
    `// Per year: bits 0..12 = 30-day months by 0-based ordinal, bits 13..16 = 1-based ordinal of the leap month (0: none).\n` +
    `constexpr int32_t k${prefix}FirstYear = ${LUNISOLAR_FIRST};\n` +
    `constexpr int32_t k${prefix}LastYear = ${LUNISOLAR_LAST};\n` +
    `// The epoch day of month 1 day 1 of k${prefix}FirstYear; every later year starts where the previous one ends.\n` +
    `constexpr int64_t k${prefix}FirstNewYear = ${first};\n` +
    `constexpr uint32_t k${prefix}Years[] = {\n${rows(packed, 8, 5)}\n};\n`;
  fs.writeFileSync(path.join(outDir, `calendar_${calendar}_data.inc`), text);
  for (const line of differ) console.error(`${calendar} differs from ICU: ${line}`);
  for (const line of edges) console.error(`${calendar} edge (< ${EDGE_MINUTES} min from midnight): ${line}`);
  return packed.length;
}

function writeUmalqura() {
  const { first, packed } = umalqura();
  const text = header('calendar_islamic_umalqura_data.inc', `islamic-umalqura month table, AH ${UMALQURA_FIRST}..${UMALQURA_LAST}`) +
    `// Per AH year: bit i = month i+1 has 30 days (else 29). Outside these years: the islamic-civil arithmetic (as ICU).\n` +
    `constexpr int32_t kUmalquraFirstYear = ${UMALQURA_FIRST};\n` +
    `constexpr int32_t kUmalquraLastYear = ${UMALQURA_LAST};\n` +
    `// The epoch day of 1 Muharram kUmalquraFirstYear; every later year starts where the previous one ends.\n` +
    `constexpr int64_t kUmalquraFirstNewYear = ${first};\n` +
    `constexpr uint16_t kUmalquraYears[] = {\n${rows(packed, 10, 3)}\n};\n`;
  fs.writeFileSync(path.join(outDir, 'calendar_islamic_umalqura_data.inc'), text);
  return packed.length;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  fs.mkdirSync(outDir, { recursive: true });
  const counts = { chinese: writeLunisolar('chinese', 'Chinese'), dangi: writeLunisolar('dangi', 'Dangi'), umalqura: writeUmalqura() };
  console.error(JSON.stringify({ outDir, years: counts }));
}
