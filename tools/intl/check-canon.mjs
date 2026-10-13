// check-canon.mjs: compare the engine's Intl.getCanonicalLocales and Intl.Locale maximize/minimize with node's on
// tags built from the alias and likely-subtags data. Differences are printed (ICU and CLDR 48 disagree on a few).
//   node tools/intl/check-canon.mjs ./engine
import { execFileSync } from 'node:child_process';
import { writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const engine = process.argv[2] ?? './engine';
const alias = require('cldr-core/supplemental/aliases.json').supplemental.metadata.alias;
const likely = require('cldr-core/supplemental/likelySubtags.json').supplemental.likelySubtags;

const tags = new Set();
for (const t of Object.keys(alias.languageAlias)) tags.add(t), tags.add(`${t}-fonipa`), tags.add(`${t}-SU`);
for (const t of Object.keys(alias.territoryAlias)) tags.add(`und-${t}`), tags.add(`ru-${t}`), tags.add(`hy-Armn-${t}`);
for (const t of Object.keys(alias.scriptAlias)) tags.add(`en-${t}`);
for (const t of Object.keys(alias.variantAlias)) tags.add(`ja-Latn-${t}`), tags.add(`ja-hepburn-${t}`);
for (const t of Object.keys(alias.subdivisionAlias)) tags.add(`und-u-sd-${t}`), tags.add(`en-u-rg-${t}zzzz`);
for (const [k, v] of Object.entries(likely)) tags.add(k), tags.add(v);
const valid = [...tags].filter((t) => { try { Intl.getCanonicalLocales(t); return true; } catch { return false; } });
const expect = (t) => {
  const c = Intl.getCanonicalLocales(t)[0];
  const l = new Intl.Locale(c);
  return [c, l.maximize().toString(), l.minimize().toString()].join(' ');
};
const script = `const tags = ${JSON.stringify(valid)};\nfor (const t of tags) { const c = Intl.getCanonicalLocales(t)[0]; const l = new Intl.Locale(c);\n  print(t + ' ' + [c, l.maximize().toString(), l.minimize().toString()].join(' ')); }\n`;
writeFileSync('/tmp/check-canon.js', script);
const out = execFileSync('sh', ['-c', `ulimit -v 2000000; ${engine} /tmp/check-canon.js`], { encoding: 'utf8', maxBuffer: 1 << 26 });
let bad = 0;
const kinds = [0, 0, 0];
for (const line of out.trim().split('\n')) {
  const [t, ...got] = line.split(' ');
  const want = expect(t).split(' ');
  for (let i = 0; i < 3; i++) {
    if (got[i] !== want[i]) {
      kinds[i]++;
      if (bad++ < 40) console.log(`${t}: ${['canonical', 'maximize', 'minimize'][i]} got ${got[i]} want ${want[i]}`);
    }
  }
}
console.log(`${valid.length} tags; differences: canonical ${kinds[0]}, maximize ${kinds[1]}, minimize ${kinds[2]}`);
