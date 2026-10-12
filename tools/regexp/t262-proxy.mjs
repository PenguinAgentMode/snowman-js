// t262-proxy.mjs: what validate() alone does for test262's parse expectations, before the parser can run them.
//   node tools/regexp/t262-proxy.mjs <dir>...
// A parse-negative file counts as rejected if validate() rejects one of its regex literals; a positive file as
// parsing if validate() accepts all of them. Files without regex literals are left out.
import path from 'node:path';
import { readdirSync, statSync } from 'node:fs';

import { runDriver } from './driver.mjs';
import { extract } from './extract.mjs';

function* walk(p) {
  if (statSync(p).isDirectory()) {
    for (const e of readdirSync(p).sort()) yield* walk(path.join(p, e));
  } else if (p.endsWith('.js') && !p.includes('_FIXTURE')) {
    yield p;
  }
}

for (const dir of process.argv.slice(2)) {
  const files = [...walk(dir)].map((f) => ({ f, cases: extract(f).filter((c) => c.kind === 'literal') })).filter((x) => x.cases.length);
  const all = files.flatMap((x) => x.cases);
  const verdicts = runDriver(all);
  let k = 0;
  const neg = { n: 0, ok: 0, bad: [] };
  const pos = { n: 0, ok: 0, bad: [] };
  for (const { f, cases } of files) {
    const v = cases.map(() => verdicts[k++]);
    const negative = cases[0].negative;
    const bucket = negative ? neg : pos;
    bucket.n++;
    const ok = negative ? v.some((x) => !x.startsWith('1')) : v.every((x) => x.startsWith('1'));
    if (ok) bucket.ok++;
    else bucket.bad.push(path.basename(f));
  }
  console.log(`${dir}: negative ${neg.ok}/${neg.n} rejected, positive ${pos.ok}/${pos.n} accepted`);
  for (const b of neg.bad) console.log(`  not rejected: ${b}`);
  for (const b of pos.bad) console.log(`  not accepted: ${b}`);
}
