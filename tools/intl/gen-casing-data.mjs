// gen-casing-data.mjs: generate src/builtins/intl/casing_data.inc, the character classes of the conditional case
// mappings of SpecialCasing.txt (tr, az, lt: After_I, More_Above, After_Soft_Dotted) for toLocale{Lower,Upper}Case.
//   cd tools/intl && npm install && node gen-casing-data.mjs
// Canonical combining classes come from node's own NFD (canonical reordering): a mark X has ccc 230 when it does not
// reorder against U+0301 (ccc 230) but does against U+0316 (ccc 220); X has a nonzero ccc when it reorders against
// one of them. Soft_Dotted comes from @unicode/unicode-17.0.0.
import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { header } from './common.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
if (process.versions.unicode !== '17.0') throw new Error(`node has Unicode ${process.versions.unicode}, the data is 17.0`);
const isMark = (s) => /^\p{M}$/u.test(s);
const nfd = (s) => s.normalize('NFD');
const above = [], nonzero = [];
for (let cp = 0x300; cp <= 0x10ffff; cp++) {
  if (cp >= 0xd800 && cp <= 0xdfff) continue;
  const x = String.fromCodePoint(cp);
  if (!isMark(x) || nfd(x) !== x) continue;
  const equal = (m) => nfd(`a${x}${m}`) === nfd(`a${m}${x}`);
  const is230 = !equal('\u0301') && equal('\u0316');
  const isNonzero = is230 || equal('\u0301') || equal('\u0316');
  if (is230) above.push(cp);
  if (isNonzero) nonzero.push(cp);
}
const soft = (await import('@unicode/unicode-17.0.0/Binary_Property/Soft_Dotted/code-points.mjs')).default;
function ranges(cps) {
  const out = [];
  for (const cp of [...cps].sort((a, b) => a - b)) {
    const last = out.at(-1);
    if (last && last[1] === cp - 1) last[1] = cp;
    else out.push([cp, cp]);
  }
  return out;
}
const table = (name, cps, comment) => {
  const rs = ranges(cps);
  const lines = [`// ${comment}`, `static constexpr CodeRange ${name}[] = {`];
  for (let i = 0; i < rs.length; i += 8) lines.push('    ' + rs.slice(i, i + 8).map(([a, b]) => `{0x${a.toString(16)}, 0x${b.toString(16)}}`).join(', ') + ',');
  lines.push('};', '');
  return lines.join('\n');
};
const out = [
  header('gen-casing-data.mjs', ['@unicode/unicode-17.0.0']), '',
  table('kCombiningAbove', above, 'Canonical_Combining_Class = 230 (Above).'),
  table('kCombiningNonzero', nonzero, 'Canonical_Combining_Class != 0.'),
  table('kSoftDotted', soft, 'Soft_Dotted.'),
].join('\n');
writeFileSync(path.join(root, 'src/builtins/intl/casing_data.inc'), out);
console.log(`casing_data.inc: ${above.length} above, ${nonzero.length} nonzero, ${soft.length} soft dotted; ${(out.length / 1024).toFixed(0)} KB`);
