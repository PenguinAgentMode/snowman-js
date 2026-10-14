# Sanitizer sweep (ASan + UBSan)

Owner: hardening. The full suite under an ASan+UBSan engine, triaged by top frame.

## How

- Engine (separate worktree; the Makefile default is unchanged):
  `make -j4 CXXFLAGS='-std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' engine`.
- Needed for a clean run, all in the tree since ws/hardening: ASan fiber annotations around the coroutine switch
  (`__sanitizer_{start,finish}_switch_fiber`, src/interp/coroutine.cpp), `__asan_default_options`
  (`detect_stack_use_after_return=0`: the GC scans the real stacks, fake stacks would hide roots; `detect_leaks=0`),
  reused coroutine stacks unpoisoned, `Heap::scanRange` `no_sanitize("address")` (it reads redzones on purpose).
- Run: 4 shards one at a time, `node t262/runner/run.mjs --engine <asan engine> --suite full --shard k/4 --jobs 2
  --vm-mb 0 --mem-mb 1500`, with `ASAN_OPTIONS=log_path=$HOME/logs/asan/a` and
  `UBSAN_OPTIONS=log_path=$HOME/logs/ubsan/u:print_stacktrace=1` (one report file per process with a finding).

## Gotcha: UBSan reports in a combined ASan+UBSan build (GCC) go to stderr

- Probed with a 1-line shift overflow: with `-fsanitize=address,undefined`, UBSan ignores `log_path` (ASAN_OPTIONS
  or UBSAN_OPTIONS) and prints to stderr; a passing test's stderr is not kept, so the combined run's "0 UBSan
  reports" says nothing. `halt_on_error=1` would turn each into a failing scenario. A UBSan-only build
  (`-fsanitize=undefined`) honours `UBSAN_OPTIONS=log_path`: the UBSan sweep is a separate run with that engine.
- ASan reports do go to the log file (and abort the engine, failing the scenario); the combined run covers ASan.

## Findings

| Top frame | Kind | Tests (example) | Status |
| --- | --- | --- | --- |
| builtins/typedarray.cpp:90 InitializeTypedArrayFromTypedArray; arraybuffer.cpp:113 (slice), :149 (transfer); typedarray.cpp:465, :469 (%TypedArray%.prototype.set) | UBSan nonnull-attribute: memcpy/memmove with a null pointer (an empty buffer's data()), 0 bytes | UBSan shard 0: 27 reports in 9 processes | real (UB: GCC may assume the pointers non-null and drop later null checks). Fixed: guard each copy with its size (4747623, and the commit after it) |
| interp/class.cpp ClassDefinitionEvaluation -> DefineField | ASan heap-use-after-free: the ClassElements cell was collected mid-evaluation (only references into it were live; the conservative scan matches exact cell starts) | ASan + SNOWMAN_GC_STRESS=1, 1/40 sample: 16 class-field tests (e.g. language/statements/class/elements/intercalated-static-non-static-computed-fields.js) | real GC bug (any optimization level). Fixed: Rooted<ClassElements> (3dbab65); decorators2's file, sent to the lead |

## Runs

- Shard 0/4 (ws/hardening a42b89d engine, --jobs 2): 25,865 scenarios, 25,765 pass, 0 ASan / 0 UBSan reports.
- Shard 1/4 (--jobs 3, --mem-mb 1200): 25,638 scenarios, 25,550 pass, 1,888 s, machine guard 0, 0 ASan / 0 UBSan reports.
- Shard 2/4 (--jobs 3, --mem-mb 1200): 25,829 scenarios, 25,728 pass, 1,831 s, machine guard 0, 0 ASan reports.
- Shard 3/4 (--jobs 3, --mem-mb 1200): 25,662 scenarios, 25,572 pass, 2,805 s, machine guard 0, 0 ASan reports.
- ASan total: 0 reports over 102,994 scenarios (the full suite) at the normal GC budget.
- ASan + SNOWMAN_GC_STRESS=1, full-suite shard 0/40 (2,555 scenarios, 1 job, 1,952 s): 16 reports, one cluster (class.cpp
  below). The normal budget rarely collects inside a test, so missing roots need the stress mode to show.
- UBSan-only shards 0-2/4 (engine before the fixes): only the memcpy cluster below. Shard 3/4 on the fixed engine
  (8d966b9): 25,662 scenarios, 0 reports. UBSan sweep complete.
- ASan + SNOWMAN_GC_STRESS=1 widened pass (engine 8d966b9, --shard 0/2 of each directory, --jobs 2):
  done: 17,401 scenarios, 0 ASan reports, 16,947 pass, 454 timeouts (448 RegExp, mostly property-escapes/generated,
  plus 4 Temporal and 2 Array sort: too slow at a collection per safepoint), 0 other failures. Per directory:
  statements/class 4,374, expressions/class 3,983, GeneratorPrototype 40, AsyncGeneratorPrototype 48,
  statements/generators 276, statements/async-generator 300, for-await-of 1,207, Proxy 331, Reflect 198, Array
  sort 57, splice 84, flat 20, flatMap 21, RegExp 1,900, Temporal 4,562. Not covered: --shard 1/2 of these directories,
  and GC stress over the rest of the suite (only the 1/40 sample).
