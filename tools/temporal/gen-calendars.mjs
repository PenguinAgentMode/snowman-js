// gen-calendars.mjs: the month tables of the table-driven Temporal calendars (chinese, dangi, islamic-umalqura),
// generated from node's Intl.DateTimeFormat (ICU). Output is sorted and has no timestamps, so a re-run with the same
// node/ICU writes byte-identical files.
//
//   node tools/temporal/gen-calendars.mjs [--out src/builtins/temporal]
//
// Writes calendar_chinese_data.inc, calendar_dangi_data.inc and calendar_islamic_umalqura_data.inc.
//
// Lunisolar years (chinese, dangi): the arithmetic year is ICU's related ISO year. Per year one packed uint32:
//   bits 0..12   month i (0-based ordinal) has 30 days (else 29)
//   bits 13..16  the 1-based ordinal of the leap month (0: no leap month); a leap month after month N is "M0NL"
// plus the epoch day (days since 1970-01-01) of the first year's month 1 day 1; each later year starts where the
// previous one ends.
// Islamic-umalqura: per AH year one uint32 with bit i = month i+1 has 30 days; ICU uses its table for AH 1300..1600
// and the islamic-civil arithmetic outside it.
import fs from 'node:fs';
import path from 'node:path';

const args = process.argv.slice(2);
const outDir = args.includes('--out') ? args[args.indexOf('--out') + 1] : 'src/builtins/temporal';

const LUNISOLAR_FIRST = -6000;
const LUNISOLAR_LAST = 2200;
const UMALQURA_FIRST = 1300;
const UMALQURA_LAST = 1600;
const MS_PER_DAY = 86400000;

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

function header(name, what) {
  const v = process.versions;
  return [
    `// ${name}: ${what}`,
    '// Generated by tools/temporal/gen-calendars.mjs from node\'s Intl.DateTimeFormat; do not edit.',
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
  const { first, packed } = lunisolar(calendar);
  const text = header(`calendar_${calendar}_data.inc`, `${calendar} month table, related ISO years ${LUNISOLAR_FIRST}..${LUNISOLAR_LAST}`) +
    `// Per year: bits 0..12 = 30-day months by 0-based ordinal, bits 13..16 = 1-based ordinal of the leap month (0: none).\n` +
    `constexpr int32_t k${prefix}FirstYear = ${LUNISOLAR_FIRST};\n` +
    `constexpr int32_t k${prefix}LastYear = ${LUNISOLAR_LAST};\n` +
    `// The epoch day of month 1 day 1 of k${prefix}FirstYear; every later year starts where the previous one ends.\n` +
    `constexpr int64_t k${prefix}FirstNewYear = ${first};\n` +
    `constexpr uint32_t k${prefix}Years[] = {\n${rows(packed, 8, 5)}\n};\n`;
  fs.writeFileSync(path.join(outDir, `calendar_${calendar}_data.inc`), text);
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

fs.mkdirSync(outDir, { recursive: true });
const counts = { chinese: writeLunisolar('chinese', 'Chinese'), dangi: writeLunisolar('dangi', 'Dangi'), umalqura: writeUmalqura() };
console.error(JSON.stringify({ outDir, years: counts }));
