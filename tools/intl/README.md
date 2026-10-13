# tools/intl

Generators for the Intl data tables in `src/builtins/intl/*.inc`. Sources: node 22's own Intl (ICU 78.3, CLDR 48.0)
for locale display data, and `cldr-core` / `cldr-bcp47` 48.0.0 (pinned in package.json) for supplemental data node
does not expose (aliases, likely subtags, plural rules, BCP 47 keys). The engine never calls node or ICU at runtime.

```
cd tools/intl && npm install
node gen-locale-tables.mjs     # src/builtins/intl/locale_tables.inc
```

Each .inc names its generator, command and the node/ICU/CLDR/tz/Unicode versions in its header. Output is
deterministic (sorted, no timestamps). Reproducibility check: re-run every generator, then
`git diff --exit-code src/builtins/intl/`.
