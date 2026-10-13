#!/usr/bin/env node
// check-tz.mjs: compares the engine's time zones with node's own (ICU) data through the public Temporal API.
//
//   node tools/temporal/check-tz.mjs [./engine]
//
// For every primary zone: offsetNanoseconds at 60 pseudo-random instants from 1800 to 2400 (past the explicit
// data, where the annual rules apply), and every transition the engine reports from 1850 to 2110 through
// getTimeZoneTransition("next"), each checked against node: the offset must change exactly there. (That no change
// is missed in between is gen-tz.mjs's job: it bisects every change node shows.) Exits 1 on any mismatch.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const engine = process.argv[2] ?? './engine';
const fmt = new Map();
function offsetAt(zone, ms) {
  let f = fmt.get(zone);
  if (!f) {
    f = new Intl.DateTimeFormat('en-US', {
      timeZone: zone, hourCycle: 'h23', year: 'numeric', month: 'numeric', day: 'numeric',
      hour: 'numeric', minute: 'numeric', second: 'numeric',
    });
    fmt.set(zone, f);
  }
  const p = {};
  for (const x of f.formatToParts(ms)) p[x.type] = x.value;
  return Date.UTC(+p.year, p.month - 1, +p.day, +p.hour, +p.minute, +p.second) - Math.floor(ms / 1000) * 1000;
}

const zones = Intl.supportedValuesOf('timeZone');
let seed = 1;
const random = () => (seed = (seed * 48271) % 2147483647) / 2147483647;
const lo = Date.UTC(1800, 0, 1), hi = Date.UTC(2400, 0, 1);
const samples = zones.map(() => Array.from({ length: 60 }, () => Math.floor((lo + random() * (hi - lo)) / 1000) * 1000));

// The engine prints, per zone, the offsets (ms) at the samples, then the transitions (epoch ms).
const script = `
const zones = ${JSON.stringify(zones)};
const samples = ${JSON.stringify(samples)};
const start = BigInt(Date.UTC(1850, 0, 1)) * 1000000n, end = BigInt(Date.UTC(2110, 0, 1)) * 1000000n;
for (let i = 0; i < zones.length; i++) {
  const offsets = samples[i].map((ms) => new Temporal.ZonedDateTime(BigInt(ms) * 1000000n, zones[i]).offsetNanoseconds / 1e6);
  const transitions = [];
  for (let z = new Temporal.ZonedDateTime(start, zones[i]); ; ) {
    z = z.getTimeZoneTransition("next");
    if (!z || z.epochNanoseconds >= end) break;
    transitions.push(z.epochMilliseconds);
  }
  print(JSON.stringify([zones[i], offsets, transitions]));
}`;
const file = path.join(os.tmpdir(), `check-tz-${process.pid}.js`);
fs.writeFileSync(file, script);
const out = execFileSync(engine, [file], { maxBuffer: 1 << 28 }).toString().trim().split('\n');
fs.unlinkSync(file);

let failures = 0, offsets = 0, transitions = 0;
const fail = (m) => { if (failures++ < 20) console.log('FAIL', m); };
for (const [i, line] of out.entries()) {
  const [zone, got, trans] = JSON.parse(line);
  if (zone !== zones[i]) fail(`zone order ${zone} ${zones[i]}`);
  got.forEach((o, k) => { offsets++; if (o !== offsetAt(zone, samples[i][k])) fail(`${zone} @${samples[i][k]}: ${o} vs node ${offsetAt(zone, samples[i][k])}`); });
  for (const t of trans) {
    transitions++;
    if (offsetAt(zone, t - 1000) === offsetAt(zone, t)) fail(`${zone}: no offset change at ${new Date(t).toISOString()}`);
  }
}
console.log(`${zones.length} zones, ${offsets} offsets, ${transitions} transitions checked: ${failures} mismatches`);
process.exit(failures ? 1 : 0);
