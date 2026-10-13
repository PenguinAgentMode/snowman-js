## Full suite, old runner (main 19dc921, rep-1) vs new runner (ws/runner-strict 5606282), same binary 19dc92105

files 53616 / 53616; passing before 53452, after 53465; newly passing 13, newly failing 0; failing in both with a different error 1; missing 0
scenarios passing before 102698, after 102712; HostScriptError scenarios before 0, after 0; engine time (sum of ms) before 8255 s, after 12383 s

### newly failing

### newly passing
test/staging/sm/strict/10.4.2.js  (before: strict fail Test262Error: Expected SameValue(«false», «true») to be true)
test/staging/sm/strict/10.6.js  (before: strict fail Test262Error: Expected SameValue(«false», «true») to be true)
test/staging/sm/strict/12.10.1.js  (before: strict fail Test262Error: Expected SameValue(«false», «true») to be true)
test/staging/sm/strict/15.10.7.js  (before: strict fail Test262Error: Expected SameValue(«false», «true») to be true)
test/staging/sm/strict/15.3.5.1.js  (before: strict fail Test262Error: Expected SameValue(«false», «true») to be true)
test/staging/sm/strict/15.3.5.2.js  (before: strict fail Test262Error: Expected SameValue(«false», «true») to be true)
test/staging/sm/strict/15.5.5.1.js  (before: strict fail Test262Error: Expected SameValue(«false», «true») to be true)
test/staging/sm/strict/15.5.5.2.js  (before: strict fail Test262Error: Expected SameValue(«false», «true») to be true)
test/staging/sm/strict/8.12.5.js  (before: strict fail Test262Error: Expected SameValue(«false», «true») to be true)
test/staging/sm/strict/8.12.7-2.js  (before: strict fail Test262Error: Expected SameValue(«false», «true») to be true)
test/staging/sm/strict/8.12.7.js  (before: strict fail Test262Error: Expected SameValue(«false», «true») to be true)
test/staging/sm/strict/8.7.2.js  (before: strict fail Test262Error: Expected SameValue(«false», «true») to be true)
test/staging/sm/strict/eval-variable-environment.js  (before: strict fail Test262Error: Expected SameValue(«false», «true») to be true)

### failing in both, different error
test/staging/sm/RegExp/unicode-braced.js
    before: sloppy fail fatal: out of memory; strict fail fatal: out of memory
    after:  strict fail fatal: out of memory
