// check-calendars.mjs: checks the generated calendar tables (tools/temporal/gen-calendars.mjs).
//
//   node tools/temporal/check-calendars.mjs [--dir src/builtins/temporal] [--engine ./engine] [--samples N]
//
// 1. Regenerates the tables into $TMPDIR and requires them byte-identical to the checked-in ones (catches a stale table
//    or a different node/ICU).
// 2. Decodes the checked-in tables on their own and compares calendar dates against Intl.DateTimeFormat: every day of
//    a few windows (1900, 2000, 2100, the earliest years) plus N deterministic random days across each table.
// 3. Checks that ICU's islamic-umalqura equals islamic-civil outside the table (our fallback), on sample days.
// 4. With --engine: runs the engine on the same sample days through Temporal.PlainDate and compares year, monthCode
//    and day with Intl.
// Prints one summary line per check; exits 1 on any mismatch.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (name, fallback) => (args.includes(name) ? args[args.indexOf(name) + 1] : fallback);
const dir = opt('--dir', 'src/builtins/temporal');
const engine = opt('--engine', null);
const samples = Number(opt('--samples', 4000));
const MS_PER_DAY = 86400000;
const FILES = ['calendar_chinese_data.inc', 'calendar_dangi_data.inc', 'calendar_islamic_umalqura_data.inc'];
let failures = 0;

function report(name, bad, total, examples) {
  console.log(`${bad === 0 ? 'ok  ' : 'FAIL'} ${name}: ${total - bad} / ${total}${examples.length ? ' e.g. ' + examples.slice(0, 3).join('; ') : ''}`);
  if (bad) failures++;
}

// 1. Byte-identical regeneration.
{
  const tmp = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'calendars-'));
  execFileSync(process.execPath, [path.join(here, 'gen-calendars.mjs'), '--out', tmp], { stdio: ['ignore', 'ignore', 'ignore'] });
  const differ = FILES.filter((f) => !fs.readFileSync(path.join(dir, f)).equals(fs.readFileSync(path.join(tmp, f))));
  fs.rmSync(tmp, { recursive: true, force: true });
  report('regenerated tables are byte-identical', differ.length, FILES.length, differ);
}

// 2. Decode the checked-in tables.
function constant(text, name) {
  const m = text.match(new RegExp(`${name} = (-?\\d+);`));
  return Number(m[1]);
}
function array(text, name) {
  const body = text.match(new RegExp(`${name}\\[\\] = \\{([^}]*)\\}`))[1];
  return body.split(',').map((s) => s.trim()).filter(Boolean).map(Number);
}

function lunisolarTable(file, prefix) {
  const text = fs.readFileSync(path.join(dir, file), 'utf8');
  const firstYear = constant(text, `k${prefix}FirstYear`);
  const years = array(text, `k${prefix}Years`);
  let start = constant(text, `k${prefix}FirstNewYear`);
  const rows = years.map((packed, i) => {
    const leap = (packed >> 13) & 15;
    const count = leap ? 13 : 12;
    const lengths = Array.from({ length: count }, (_, m) => (packed & (1 << m) ? 30 : 29));
    const row = { year: firstYear + i, start, leap, lengths };
    start += lengths.reduce((a, b) => a + b, 0);
    return row;
  });
  return { first: rows[0].start, end: start, rows };
}

function umalquraTable() {
  const text = fs.readFileSync(path.join(dir, 'calendar_islamic_umalqura_data.inc'), 'utf8');
  const firstYear = constant(text, 'kUmalquraFirstYear');
  let start = constant(text, 'kUmalquraFirstNewYear');
  const rows = array(text, 'kUmalquraYears').map((bits, i) => {
    const lengths = Array.from({ length: 12 }, (_, m) => (bits & (1 << m) ? 30 : 29));
    const row = { year: firstYear + i, start, leap: 0, lengths };
    start += lengths.reduce((a, b) => a + b, 0);
    return row;
  });
  return { first: rows[0].start, end: start, rows };
}

// The calendar date of `epochDay` from a decoded table: { year, monthCode, day }.
function lookup(table, epochDay) {
  let lo = 0, hi = table.rows.length - 1;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (table.rows[mid].start <= epochDay) lo = mid; else hi = mid - 1;
  }
  const row = table.rows[lo];
  let day = epochDay - row.start;
  let m = 0;
  while (day >= row.lengths[m]) day -= row.lengths[m++];
  const ordinal = m + 1;
  let number = ordinal;
  let leap = false;
  if (row.leap) {
    if (ordinal === row.leap) { number = ordinal - 1; leap = true; } else if (ordinal > row.leap) number = ordinal - 1;
  }
  return { year: row.year, monthCode: 'M' + String(number).padStart(2, '0') + (leap ? 'L' : ''), day: day + 1 };
}

function intlDate(calendar) {
  const f = new Intl.DateTimeFormat(`en-u-ca-${calendar}`, { timeZone: 'UTC', year: 'numeric', month: 'numeric', day: 'numeric' });
  return (epochDay) => {
    const p = {};
    for (const x of f.formatToParts(new Date(epochDay * MS_PER_DAY))) p[x.type] = x.value;
    const leap = p.month.endsWith('bis');
    const number = Number(leap ? p.month.slice(0, -3) : p.month);
    let year = Number(p.relatedYear ?? p.year);
    if (p.era === 'BH') year = 1 - year;
    return { year, monthCode: 'M' + String(number).padStart(2, '0') + (leap ? 'L' : ''), day: Number(p.day) };
  };
}

function epochDayOfIso(year, month, day) {
  const d = new Date(0);
  d.setUTCFullYear(year, month - 1, day);
  return Math.round(d.getTime() / MS_PER_DAY);
}

// Deterministic sample days: whole windows plus pseudo-random days in [first, end).
function sampleDays(first, end, windows) {
  const days = new Set();
  for (const [from, to] of windows) for (let d = Math.max(from, first); d < Math.min(to, end); d++) days.add(d);
  let s = 0x9e3779b9;
  for (let i = 0; i < samples; i++) {
    s = (Math.imul(s ^ (s >>> 15), 0x2c1b3c6d) + 0x6d2b79f5) >>> 0;
    days.add(first + (s % (end - first)));
  }
  return [...days].sort((a, b) => a - b);
}

const same = (a, b) => a.year === b.year && a.monthCode === b.monthCode && a.day === b.day;
const show = (d, a, b) => `${d}: table ${a.year}-${a.monthCode}-${a.day} intl ${b.year}-${b.monthCode}-${b.day}`;
const checked = [];

for (const [calendar, table] of [
  ['chinese', lunisolarTable('calendar_chinese_data.inc', 'Chinese')],
  ['dangi', lunisolarTable('calendar_dangi_data.inc', 'Dangi')],
  ['islamic-umalqura', umalquraTable()],
]) {
  const intl = intlDate(calendar);
  const windows = [[table.first, table.first + 800], [epochDayOfIso(1900, 1, 1), epochDayOfIso(1901, 3, 1)],
    [epochDayOfIso(2000, 1, 1), epochDayOfIso(2001, 3, 1)], [table.end - 800, table.end]];
  const days = sampleDays(table.first, table.end, windows);
  const bad = [];
  for (const d of days) {
    const a = lookup(table, d), b = intl(d);
    if (!same(a, b)) bad.push(show(d, a, b));
  }
  report(`${calendar} table vs Intl`, bad.length, days.length, bad);
  checked.push({ calendar, days: days.filter((_, i) => i % 4 === 0), intl });
}

// 3. islamic-umalqura falls back to islamic-civil outside its table.
{
  const umalqura = intlDate('islamic-umalqura'), civil = intlDate('islamic-civil');
  const table = umalquraTable();
  const outside = [...sampleDays(table.first - 200000, table.first, []), ...sampleDays(table.end, table.end + 200000, [])];
  const bad = outside.filter((d) => !same(umalqura(d), civil(d))).map((d) => show(d, civil(d), umalqura(d)));
  report('islamic-umalqura = islamic-civil outside the table', bad.length, outside.length, bad);
  const inside = sampleDays(table.first, table.end, []);
  const differ = inside.filter((d) => !same(umalqura(d), civil(d))).length;
  console.log(`info islamic-umalqura differs from islamic-civil on ${differ} of ${inside.length} sample days inside the table`);
}

// 4. The engine's Temporal conversions.
if (engine) {
  for (const { calendar, days, intl } of checked) {
    const script = path.join(process.env.TMPDIR || os.tmpdir(), `check-${calendar}.js`);
    fs.writeFileSync(script, `const days = ${JSON.stringify(days)};\nconst out = [];\n` +
      `for (const d of days) { const p = Temporal.PlainDate.from('1970-01-01').add({ days: d }).withCalendar(${JSON.stringify(calendar)});` +
      ` out.push(p.year + ' ' + p.monthCode + ' ' + p.day); }\nprint(out.join('|'));\n`);
    let lines = [];
    try {
      lines = execFileSync(engine, [script], { encoding: 'utf8', maxBuffer: 64 << 20 }).trim().split('|');
    } catch (e) {
      lines = [];
      console.log(`FAIL ${calendar} engine run: ${String(e.stdout || e.message).slice(0, 160)}`);
      failures++;
    }
    fs.rmSync(script, { force: true });
    if (!lines.length) continue;
    const bad = [];
    days.forEach((d, i) => {
      const [y, monthCode, day] = (lines[i] || '').split(' ');
      const a = { year: Number(y), monthCode, day: Number(day) }, b = intl(d);
      if (!same(a, b)) bad.push(show(d, a, b).replace('table', 'engine'));
    });
    report(`${calendar} engine vs Intl`, bad.length, days.length, bad);
  }
}

process.exit(failures ? 1 : 0);
