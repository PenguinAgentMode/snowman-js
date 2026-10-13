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

## Findings

| Top frame | Kind | Tests (example) | Status |
| --- | --- | --- | --- |

## Runs

- Shard 0/4 (ws/hardening a42b89d engine, --jobs 2): 25,865 scenarios, 25,765 pass, 0 ASan / 0 UBSan reports.
