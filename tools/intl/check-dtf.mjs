// check-dtf.mjs: compare the engine's Intl.DateTimeFormat output with node's for combinations of options, locales
// and instants. Differences are printed; the count of differing cases last.
//   node tools/intl/check-dtf.mjs ./engine [locale ...]
import { execFileSync } from 'node:child_process';
import { writeFileSync } from 'node:fs';

const engine = process.argv[2] ?? './engine';
const locales = process.argv.length > 3 ? process.argv.slice(3) : ['en', 'en-GB', 'de', 'fr', 'ja', 'zh', 'ko', 'es', 'ru', 'ar'];
const instants = [Date.UTC(1886, 4, 1, 14, 12, 47, 123), Date.UTC(2024, 0, 9, 0, 5, 3, 7), Date.UTC(-500, 11, 31, 23, 59, 59, 999)];
const choices = {
  weekday: [undefined, 'short', 'long'], era: [undefined, 'short'], year: [undefined, 'numeric', '2-digit'],
  month: [undefined, 'numeric', '2-digit', 'short', 'long'], day: [undefined, 'numeric'], hour: [undefined, 'numeric', '2-digit'],
  minute: [undefined, 'numeric'], second: [undefined, 'numeric'],
};
const cases = [];
const keys = Object.keys(choices);
// A deterministic sample of the combinations (every 7th), plus the styles.
let n = 0;
const walk = (i, o) => {
  if (i === keys.length) { if (n++ % 7 === 0) cases.push({ ...o }); return; }
  for (const v of choices[keys[i]]) walk(i + 1, v === undefined ? o : { ...o, [keys[i]]: v });
};
walk(0, {});
for (const d of [undefined, 'full', 'long', 'medium', 'short']) for (const t of [undefined, 'full', 'long', 'medium', 'short']) if (d || t) cases.push({ dateStyle: d, timeStyle: t });
for (const hc of ['h11', 'h12', 'h23', 'h24']) cases.push({ hour: 'numeric', minute: 'numeric', hourCycle: hc }), cases.push({ timeStyle: 'short', hourCycle: hc });
cases.push({ hour12: true, hour: 'numeric' }, { hour12: false, hour: 'numeric' }, { dayPeriod: 'short', hour: 'numeric' }, { timeZoneName: 'short' },
  { timeZoneName: 'longOffset', hour: 'numeric' }, { fractionalSecondDigits: 2, second: 'numeric' });
const json = (o) => JSON.stringify(o, (k, v) => (v === undefined ? undefined : v));
const script = `const cases = ${json(cases)}; const locales = ${JSON.stringify(locales)}; const instants = ${JSON.stringify(instants)};
for (const l of locales) for (const c of cases) { let out; try { const f = new Intl.DateTimeFormat(l, { timeZone: 'UTC', ...c }); out = instants.map((t) => f.format(t)).join(' | '); } catch (e) { out = 'ERR ' + e.name; } print(JSON.stringify(out)); }`;
writeFileSync('/tmp/check-dtf.js', script);
const got = execFileSync('sh', ['-c', `ulimit -v 2000000; ${engine} /tmp/check-dtf.js`], { encoding: 'utf8', maxBuffer: 1 << 26 }).trim().split('\n').map((l) => JSON.parse(l));
let i = 0, bad = 0;
for (const l of locales) {
  for (const c of cases) {
    let want;
    try { const f = new Intl.DateTimeFormat(l, { timeZone: 'UTC', ...c }); want = instants.map((t) => f.format(t)).join(' | '); } catch (e) { want = 'ERR ' + e.name; }
    if (got[i] !== want && bad++ < 60) console.log(`${l} ${json(c)}\n   got  ${JSON.stringify(got[i]).replace(/[\u0080-\uffff]/g, (ch) => `\\u${ch.charCodeAt(0).toString(16)}`)}\n   want ${JSON.stringify(want).replace(/[\u0080-\uffff]/g, (ch) => `\\u${ch.charCodeAt(0).toString(16)}`)}`);
    i++;
  }
}
console.log(`${i} cases, ${bad} differ`);
