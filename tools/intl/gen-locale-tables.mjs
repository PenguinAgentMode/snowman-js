// gen-locale-tables.mjs: generate src/builtins/intl/locale_tables.inc, the language-tag data of
// src/builtins/intl/locale_id.cpp (UTS 35 alias replacement, likely subtags, u/t extension value aliases).
//   cd tools/intl && npm install && node gen-locale-tables.mjs
// Sources: cldr-core and cldr-bcp47 48.0.0, the CLDR version of node 22's ICU (process.versions.cldr).
import { writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

import { cmp, header } from './common.mjs';

const require = createRequire(import.meta.url);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const supplemental = (name) => require(`cldr-core/supplemental/${name}.json`).supplemental;
const bcp47 = (name) => require(`cldr-bcp47/bcp47/${name}.json`).keyword;

if (process.versions.cldr !== '48.0') throw new Error(`node has CLDR ${process.versions.cldr}, the data is CLDR 48.0`);

const alias = supplemental('aliases').metadata.alias;

// Subtag shapes (UTS 35 unicode_language_id).
const isLanguage = (s) => /^([a-z]{2,3}|[a-z]{5,8})$/i.test(s);
const isScript = (s) => /^[a-z]{4}$/i.test(s);
const isRegion = (s) => /^([a-z]{2}|[0-9]{3})$/i.test(s);
const isVariant = (s) => /^([a-z0-9]{5,8}|[0-9][a-z0-9]{3})$/i.test(s);
// A unicode_language_id with no duplicate variants, or null.
function parseLanguageId(tag) {
  const parts = tag.split('-');
  let i = 0;
  if (!isLanguage(parts[i] ?? '')) return null;
  const id = { language: parts[i++].toLowerCase(), script: '', region: '', variants: [] };
  if (i < parts.length && isScript(parts[i])) id.script = parts[i][0].toUpperCase() + parts[i++].slice(1).toLowerCase();
  if (i < parts.length && isRegion(parts[i])) id.region = parts[i++].toUpperCase();
  while (i < parts.length && isVariant(parts[i])) id.variants.push(parts[i++].toLowerCase());
  if (i !== parts.length || new Set(id.variants).size !== id.variants.length) return null;
  return id;
}
const idString = (id) => [id.language, id.script, id.region, ...id.variants.sort()].filter(Boolean).join('-');

const sortedPairs = (pairs) => {
  const seen = new Map();
  for (const [a, b] of pairs) {
    if (seen.has(a) && seen.get(a) !== b) throw new Error(`conflicting ${a}: ${seen.get(a)} / ${b}`);
    seen.set(a, b);
  }
  return [...seen].sort((x, y) => cmp(x[0], y[0]));
};
const pairTable = (name, pairs, comment) =>
  `// ${comment}\nstatic constexpr AliasEntry ${name}[] = {\n` +
  sortedPairs(pairs).map(([a, b]) => `    {"${a}", "${b}"},`).join('\n') + '\n};\n';

// languageAlias: simple rules (one language subtag) and complex ones (with region and/or variants). Rules whose
// source is not a structurally valid unicode_language_id (extlangs, grandfathered i-*, sgn-BE-FR) never match.
const simpleLanguage = [];
const complexLanguage = [];
for (const [type, { _replacement: replacement }] of Object.entries(alias.languageAlias)) {
  const source = parseLanguageId(type);
  if (!source) continue;
  if (source.script) throw new Error(`languageAlias with a script: ${type}`);
  const target = replacement.split('-x-')[0];  // "en-x-i-default" style targets only come with invalid sources
  if (!parseLanguageId(target)) throw new Error(`bad replacement ${replacement} for ${type}`);
  if (!source.region && !source.variants.length) simpleLanguage.push([source.language, target]);
  else complexLanguage.push([idString(source), target]);
}
const script = Object.entries(alias.scriptAlias).map(([t, v]) => [t, v._replacement]);
const region = Object.entries(alias.territoryAlias).map(([t, v]) => [t, v._replacement]);
const variant = Object.entries(alias.variantAlias).map(([t, v]) => [t, v._replacement]);
const subdivision = Object.entries(alias.subdivisionAlias).map(([t, v]) => [t, v._replacement]);

// u and t extension values: aliases (that are well-formed values) and deprecated types with a preferred one.
const isUValue = (s) => /^[a-z0-9]{3,8}(-[a-z0-9]{3,8})*$/.test(s);
function valueAliases(keyword) {
  const out = [];
  for (const [key, types] of Object.entries(keyword)) {
    for (const [type, info] of Object.entries(types)) {
      if (type.startsWith('_')) continue;
      if (info._preferred) out.push([`${key}-${type}`, info._preferred]);
      if (info._deprecated) continue;
      for (const a of (info._alias ?? '').split(' ')) {
        const value = a.toLowerCase();
        if (a && isUValue(value) && value !== type) out.push([`${key}-${value}`, type]);
      }
    }
  }
  return out;
}
const uFiles = ['calendar', 'collation', 'currency', 'measure', 'number', 'segmentation', 'timezone', 'variant'];
const uAliases = uFiles.flatMap((f) => valueAliases(bcp47(f).u));
const tAliases = ['transform', 'transform-destination'].flatMap((f) => valueAliases(bcp47(f).t));
for (const [k, v] of [...uAliases, ...tAliases]) if (!isUValue(v)) throw new Error(`bad canonical value ${k} -> ${v}`);

// Likely subtags, one "from to" line each, sorted by from.
const likely = sortedPairs(Object.entries(supplemental('likelySubtags').likelySubtags));
for (const [from, to] of likely) if (!parseLanguageId(from) || !parseLanguageId(to)) throw new Error(`likely ${from} ${to}`);
const likelyBlob = likely.map(([a, b]) => `    "${a} ${b}\\n"`).join('\n');

const out = [
  header('gen-locale-tables.mjs'),
  '// Every table is sorted by its first column (byte order) for binary search.',
  '',
  pairTable('kLanguageAliases', simpleLanguage, 'languageAlias with a single language subtag as source.'),
  pairTable('kComplexLanguageAliases', complexLanguage, 'languageAlias with region and/or variant subtags (language may be "und").'),
  pairTable('kScriptAliases', script, 'scriptAlias.'),
  pairTable('kRegionAliases', region, 'territoryAlias; several replacements are separated by spaces.'),
  pairTable('kVariantAliases', variant, 'variantAlias.'),
  pairTable('kSubdivisionAliases', subdivision, 'subdivisionAlias (for the u-rg and u-sd values); several replacements are separated by spaces.'),
  pairTable('kUnicodeValueAliases', uAliases, 'u extension "key-alias" -> canonical type (bcp47 alias and deprecated/preferred).'),
  pairTable('kTransformValueAliases', tAliases, 't extension "tkey-alias" -> canonical tvalue.'),
  '// likelySubtags: "from to\\n" lines.',
  'static constexpr char kLikelySubtags[] =',
  likelyBlob + ';',
  '',
].join('\n');
writeFileSync(path.join(root, 'src/builtins/intl/locale_tables.inc'), out);
console.log(`locale_tables.inc: ${simpleLanguage.length} + ${complexLanguage.length} language, ${region.length} region, ` +
  `${subdivision.length} subdivision, ${uAliases.length} u, ${tAliases.length} t aliases; ${likely.length} likely subtags`);
