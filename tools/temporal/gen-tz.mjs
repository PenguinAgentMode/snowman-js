#!/usr/bin/env node
// gen-tz.mjs: the IANA time zone tables for Temporal (src/builtins/temporal/tz_data.inc), derived from node's own
// ICU time zone data. Nothing here runs at engine runtime.
//
//   node tools/temporal/gen-tz.mjs                        writes src/builtins/temporal/tz_data.inc (~2 min, 4 workers)
//   node tools/temporal/gen-tz.mjs --names-from <tzdb>    first refreshes tools/temporal/tz-names.txt from a tzdb
//                                                         checkout (Zone/Link lines of the data files + backzone)
//
// Reproducible: the output depends only on tz-names.txt and the node binary (its ICU/tz/CLDR versions are in the
// header of the .inc); everything is sorted and there are no timestamps. To check a checked-in table, run with
// the node version its header names, then: git diff --exit-code src/builtins/temporal/tz_data.inc
//
// Identifiers: every name in tz-names.txt that node's Intl.DateTimeFormat accepts, plus
// Intl.supportedValuesOf("timeZone"). Names that ICU resolves to the same zone share one zone record, so a
// Link equals its Zone (TimeZoneEquals). The primary identifier of a group is its tzdb Zone (from the main data
// files, else from backzone), else ICU's canonical name; "UTC" for the UTC group (ECMA-402 requires it).
//
// Offsets: for each group, the UTC offset (whole seconds) is probed once a day from 1800-01-01 to 2101-01-01
// by formatting the wall-clock time with Intl.DateTimeFormat, and every change is bisected to the second.
// Before 1800 the first offset applies (no tzdb zone changes before 1800 except LMT oddities). After 2100:
//   - a zone whose last transitions follow a fixed annual rule (two transitions a year, each "the first weekday
//     on or after day N", "the last weekday" or a fixed date of a month, at a wall-clock time) stores that rule,
//     verified against node for every year from where it starts (at least 10 years) to 2100; it then continues
//     forever, like tzdb's POSIX TZ footer;
//   - any other zone keeps its explicit transitions up to 2100 and its last offset after that (truncated).
import fs from 'node:fs';
import path from 'node:path';
import { Worker, isMainThread, parentPort, workerData } from 'node:worker_threads';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../..');
const namesFile = path.join(here, 'tz-names.txt');
const outFile = path.join(root, 'src/builtins/temporal/tz_data.inc');

const START = Date.UTC(1800, 0, 1) / 1000;
const END = Date.UTC(2101, 0, 1) / 1000;
const DAY = 86400;
const LAST_RULE_YEAR = 2100;

function makeOffsetAt(zone) {
  const f = new Intl.DateTimeFormat('en-US', {
    timeZone: zone, hourCycle: 'h23', year: 'numeric', month: 'numeric', day: 'numeric',
    hour: 'numeric', minute: 'numeric', second: 'numeric',
  });
  return (sec) => {
    const p = {};
    for (const x of f.formatToParts(sec * 1000)) p[x.type] = x.value;
    return (Date.UTC(+p.year, p.month - 1, +p.day, +p.hour, +p.minute, +p.second) / 1000) - sec;
  };
}

// The transitions of one zone in [START, END): {initial, transitions: [[sec, offsetAfter], ...]}.
function probe(zone) {
  const offsetAt = makeOffsetAt(zone);
  const initial = offsetAt(START);
  const transitions = [];
  let prev = initial;
  for (let t = START + DAY; t < END + DAY; t += DAY) {
    const cur = offsetAt(t);
    let lo = t - DAY, offLo = prev;
    while (offLo !== cur) {
      let a = lo, b = t;  // offsetAt(a) === offLo, offsetAt(b) !== offLo
      while (b - a > 1) {
        const m = Math.floor((a + b) / 2);
        if (offsetAt(m) === offLo) a = m; else b = m;
      }
      const o = offsetAt(b);
      if (b < END) transitions.push([b, o]);
      lo = b;
      offLo = o;
    }
    prev = cur;
  }
  return { initial, transitions };
}

// ---- rules ---------------------------------------------------------------------------------------------------

const daysInMonth = (y, m) => new Date(Date.UTC(y, m, 0)).getUTCDate();  // m is 1-based
const dayStart = (y, m, d) => Date.UTC(y, m - 1, d) / 1000;
const yearOf = (sec) => new Date(sec * 1000).getUTCFullYear();

// A transition descriptor: in month `month`, on day `day` (weekday < 0) or on the first `weekday` on or after
// `day` (day 0 = the last `weekday` of the month), at `secs` seconds after local midnight in the offset in
// effect before the transition; the new offset is `offset`.
function ruleInstant(r, y, offsetBefore) {
  let d;
  if (r.weekday < 0) d = r.day;
  else if (r.day === 0) {
    const last = daysInMonth(y, r.month);
    const wd = new Date(Date.UTC(y, r.month - 1, last)).getUTCDay();
    d = last - ((wd - r.weekday + 7) % 7);
  } else {
    const wd = new Date(Date.UTC(y, r.month - 1, r.day)).getUTCDay();
    d = r.day + ((r.weekday - wd + 7) % 7);
  }
  return dayStart(y, r.month, d) + r.secs - offsetBefore;
}

function candidates(sec, offsetBefore, offset) {
  const out = [];
  const local = sec + offsetBefore;
  for (const shift of [0, -1, 1]) {
    const dayLocal = Math.floor(local / DAY) + shift;
    const secs = local - dayLocal * DAY;
    const dt = new Date(dayLocal * DAY * 1000);
    const y = dt.getUTCFullYear(), month = dt.getUTCMonth() + 1, d = dt.getUTCDate(), wd = dt.getUTCDay();
    out.push({ month, day: d, weekday: -1, secs, offset });
    if (d > daysInMonth(y, month) - 7) out.push({ month, day: 0, weekday: wd, secs, offset });
    for (let first = Math.max(1, d - 6); first <= d; first++) out.push({ month, day: first, weekday: wd, secs, offset });
  }
  return out;
}

// The rule {a, b, startYear} that reproduces this zone's transitions from startYear to 2100, or null.
function inferRule({ transitions }) {
  const byYear = new Map();
  for (const [sec, off] of transitions) {
    const y = yearOf(sec);
    if (!byYear.has(y)) byYear.set(y, []);
    byYear.get(y).push([sec, off]);
  }
  const last = byYear.get(LAST_RULE_YEAR);
  if (!last || last.length !== 2) return null;
  const offA = last[0][1], offB = last[1][1];
  // How far back (contiguous years) a descriptor for slot i (0 = first in the year) keeps matching.
  const reach = (r, i) => {
    let y = LAST_RULE_YEAR;
    for (; ; y--) {
      const ts = byYear.get(y);
      if (!ts || ts.length !== 2) break;
      const before = i === 0 ? offB : offA;
      if (ts[i][1] !== r.offset || ts[(i + 1) % 2][1] !== (i === 0 ? offB : offA)) break;
      if (ruleInstant(r, y, before) !== ts[i][0]) break;
    }
    return y + 1;
  };
  let best = [null, null];
  for (const i of [0, 1]) {
    const [sec, off] = last[i];
    for (const r of candidates(sec, i === 0 ? offB : offA, off)) {
      const from = reach(r, i);
      if (!best[i] || from < best[i].from) best[i] = { r, from };
    }
  }
  const startYear = Math.max(best[0].from, best[1].from);
  if (startYear > LAST_RULE_YEAR - 10) return null;
  // The rule takes over at the start of startYear (UTC); its first transition must lie well inside the year.
  const cut = dayStart(startYear, 1, 1);
  if (ruleInstant(best[0].r, startYear, offB) < cut + 2 * DAY) return null;
  if (ruleInstant(best[1].r, startYear, offA) >= dayStart(startYear + 1, 1, 1) - 2 * DAY) return null;
  return { a: best[0].r, b: best[1].r, startYear, cut };
}

// ---- encoding -------------------------------------------------------------------------------------------------

function varint(n, out) {
  if (n < 0 || !Number.isSafeInteger(n)) throw new Error(`varint ${n}`);
  while (n >= 128) { out.push((n % 128) | 128); n = Math.floor(n / 128); }
  out.push(n);
}

async function main() {
  const args = process.argv.slice(2);
  if (args[0] === '--names-from') refreshNames(args[1]);

  // Candidate names, with their tzdb kind: Z (Zone in the main data), B (Zone only in backzone), L (Link).
  const kinds = new Map();
  for (const line of fs.readFileSync(namesFile, 'utf8').split('\n')) {
    const m = /^([ZBL]) (\S+)$/.exec(line.trim());
    if (m) kinds.set(m[2], m[1]);
  }
  const supported = Intl.supportedValuesOf('timeZone');
  for (const n of supported) if (!kinds.has(n)) kinds.set(n, 'L');

  const groups = new Map();  // ICU canonical name -> names
  for (const name of kinds.keys()) {
    let canonical;
    try {
      canonical = new Intl.DateTimeFormat('en', { timeZone: name }).resolvedOptions().timeZone;
    } catch {
      continue;
    }
    if (!groups.has(canonical)) groups.set(canonical, []);
    groups.get(canonical).push(name);
  }
  const rank = { Z: 0, B: 1, L: 2 };
  const zoneList = [...groups.keys()].sort();
  const primaryOf = new Map();
  for (const canonical of zoneList) {
    const names = groups.get(canonical);
    let primary;
    if (names.includes('UTC') && canonical === 'UTC') primary = 'UTC';
    else {
      const best = [...names].sort((x, y) => rank[kinds.get(x)] - rank[kinds.get(y)] || (x === canonical ? -1 : y === canonical ? 1 : 0) || (x < y ? -1 : 1))[0];
      primary = kinds.get(best) === 'L' ? canonical : best;
    }
    primaryOf.set(canonical, primary);
  }

  // Probe in 4 workers (GEN_TZ_CACHE=file keeps the probe results between runs while working on the encoding).
  const cache = process.env.GEN_TZ_CACHE;
  const results = new Map(cache && fs.existsSync(cache) ? JSON.parse(fs.readFileSync(cache, 'utf8')) : []);
  const todo = zoneList.filter((z) => !results.has(z));
  const jobs = 4;
  const slices = Array.from({ length: jobs }, (_, k) => todo.filter((_, i) => i % jobs === k));
  await Promise.all(slices.map((zones) => new Promise((resolve, reject) => {
    const w = new Worker(fileURLToPath(import.meta.url), { workerData: { zones } });
    w.on('message', (list) => { for (const [z, r] of list) results.set(z, r); resolve(); });
    w.on('error', reject);
  })));
  if (cache) fs.writeFileSync(cache, JSON.stringify([...results]));

  // Zones in primary-name order; names in case-insensitive order (the runtime binary-searches them).
  const zones = zoneList.map((c) => ({ canonical: c, primary: primaryOf.get(c), data: results.get(c) }))
    .sort((x, y) => (x.primary < y.primary ? -1 : 1));
  const zoneIndex = new Map(zones.map((z, i) => [z.canonical, i]));
  const names = [];
  for (const [canonical, list] of groups) for (const n of list) names.push([n, zoneIndex.get(canonical)]);
  for (const z of zones) if (!groups.get(z.canonical).includes(z.primary)) names.push([z.primary, zoneIndex.get(z.canonical)]);
  const fold = (s) => s.toLowerCase();
  names.sort((x, y) => (fold(x[0]) < fold(y[0]) ? -1 : fold(x[0]) > fold(y[0]) ? 1 : 0));

  const bytes = [];
  const offsets = [];
  const zoneRows = [];
  const ruleRows = [];
  let transitionCount = 0;
  for (const z of zones) {
    const { initial } = z.data;
    let { transitions } = z.data;
    const rule = inferRule(z.data);
    if (rule) transitions = transitions.filter(([sec]) => sec < rule.cut);
    const zoneOffsets = [...new Set([initial, ...transitions.map((t) => t[1]), ...(rule ? [rule.a.offset, rule.b.offset] : [])])];
    if (zoneOffsets.length > 255) throw new Error(`${z.primary}: too many offsets`);
    const offsetBase = offsets.length;
    offsets.push(...zoneOffsets);
    const begin = bytes.length;
    let prev = START;
    for (const [sec, off] of transitions) {
      // delta in minutes when whole (low bit 0), else in seconds (low bit 1); then the offset index.
      const delta = sec - prev;
      if (delta % 60 === 0) varint((delta / 60) * 2, bytes); else varint(delta * 2 + 1, bytes);
      bytes.push(zoneOffsets.indexOf(off));
      prev = sec;
    }
    transitionCount += transitions.length;
    let ruleIndex = -1;
    if (rule) {
      ruleIndex = ruleRows.length;
      const d = (r) => `{${r.month}, ${r.day}, ${r.weekday}, ${r.secs}, ${r.offset}}`;
      ruleRows.push(`  {${rule.cut}, ${d(rule.a)}, ${d(rule.b)}},  // ${z.primary} from ${rule.startYear}`);
    }
    zoneRows.push(`  {${names.findIndex((n) => n[0] === z.primary)}, ${offsetBase}, ${zoneOffsets.length}, ${begin}, ${bytes.length}, ${transitions.length}, ${ruleIndex}},`);
  }

  const lines = [];
  const v = process.versions;
  lines.push('// Generated by tools/temporal/gen-tz.mjs (command: node tools/temporal/gen-tz.mjs) from tools/temporal/tz-names.txt');
  lines.push(`// with node ${v.node} (ICU ${v.icu}, tz ${v.tz}, CLDR ${v.cldr}). Do not edit; re-run and git diff --exit-code to check.`);
  lines.push(`// ${names.length} identifiers, ${zones.length} zones, ${transitionCount} explicit transitions, ${ruleRows.length} annual rules.`);
  lines.push(`constexpr int64_t kTzStart = ${START};  // 1800-01-01T00:00Z: transition deltas start here`);
  lines.push(`constexpr int64_t kTzEnd = ${END};  // 2101-01-01T00:00Z: explicit data ends here`);
  lines.push('// Identifiers, ASCII-case-insensitively sorted: name, zone index.');
  lines.push('const TzName kTzNames[] = {');
  for (const [n, zi] of names) lines.push(`  {"${n}", ${zi}},`);
  lines.push('};');
  lines.push('// Zones, by primary identifier: primary name index, offsets begin, offset count, transition bytes');
  lines.push('// [begin, end), transition count, rule index (-1: none).');
  lines.push('const TzZone kTzZones[] = {');
  lines.push(...zoneRows);
  lines.push('};');
  lines.push('// UTC offsets in seconds, per zone (kTzZones[i].offsets); index 0 is the offset before the first transition.');
  lines.push('const int32_t kTzOffsets[] = {');
  for (let i = 0; i < offsets.length; i += 12) lines.push('  ' + offsets.slice(i, i + 12).join(', ') + ',');
  lines.push('};');
  lines.push('// Annual rules: cut (epoch seconds where the rule takes over), then two transitions');
  lines.push('// {month, day, weekday, secs, offset} (see TzRuleTransition).');
  lines.push('const TzRule kTzRules[] = {');
  lines.push(...ruleRows);
  lines.push('};');
  lines.push('// Transitions, base64: per transition a varint (delta from the previous transition, or from kTzStart, in');
  lines.push(`// minutes << 1, or seconds << 1 | 1), then the index of the new offset in the zone's offsets. ${bytes.length} bytes.`);
  lines.push(`constexpr size_t kTzTransitionBytes = ${bytes.length};`);
  lines.push('const char kTzTransitions64[] =');
  const b64 = Buffer.from(bytes).toString('base64');
  for (let i = 0; i < b64.length; i += 112) lines.push(`    "${b64.slice(i, i + 112)}"`);
  lines[lines.length - 1] += ';';
  fs.writeFileSync(outFile, lines.join('\n') + '\n');
  console.log(`${outFile}: ${names.length} names, ${zones.length} zones, ${transitionCount} transitions (${bytes.length} bytes), ${ruleRows.length} rules`);
}

function refreshNames(dir) {
  const read = (f) => fs.readFileSync(path.join(dir, f), 'utf8').split('\n');
  const main = ['africa', 'antarctica', 'asia', 'australasia', 'europe', 'northamerica', 'southamerica', 'etcetera', 'backward'];
  const kinds = new Map();
  for (const f of main) for (const l of read(f)) {
    const w = l.split(/\s+/);
    if (w[0] === 'Zone') kinds.set(w[1], 'Z');
  }
  for (const l of read('backzone')) {
    const w = l.split(/\s+/);
    if (w[0] === 'Zone' && !kinds.has(w[1])) kinds.set(w[1], 'B');
  }
  for (const f of [...main, 'backzone']) for (const l of read(f)) {
    const w = l.split(/\s+/);
    if (w[0] === 'Link' && !kinds.has(w[2])) kinds.set(w[2], 'L');
  }
  const out = ['# Candidate time zone identifiers for gen-tz.mjs: Z = Zone in the tzdb data files, B = Zone only in',
    `# backzone, L = Link. From tzdb ${/Release (\S+)/.exec(read('NEWS').join('\n'))?.[1] ?? '?'} (--names-from).`,
    ...[...kinds].sort((a, b) => (a[0] < b[0] ? -1 : 1)).map(([n, k]) => `${k} ${n}`)];
  fs.writeFileSync(namesFile, out.join('\n') + '\n');
}

if (!isMainThread) {
  parentPort.postMessage(workerData.zones.map((z) => [z, probe(z)]));
} else {
  await main();
}
