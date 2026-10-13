# hardening: handoff note

Owner: hardening (sanitizer sweep: ASan + UBSan over the full suite). Lead: the agent that started hardening.

## Branch / state

- Branch `ws/hardening` from main 19dc921.
- Sanitizer support (no effect on normal builds): ASan fiber annotations around snowman_coroutine_switch
  (src/interp/coroutine.{h,cpp}: resume/suspend/trampoline), __asan_default_options
  (detect_stack_use_after_return=0:detect_leaks=0, needed: the GC scans the real stacks), reused 256 KB stacks
  unpoisoned, Heap::scanRange no_sanitize("address").
- ASan engine: worktree $HOME/wt-asan, `make -j4 CXXFLAGS='-std=c++20 -O1 -g -fsanitize=address,undefined
  -fno-omit-frame-pointer' engine`.

## Run in progress

- Shard 0 (--jobs 2, --mem-mb 1500) by $HOME/logs/run-asan.sh -> asan-0.jsonl, marker asan-0.done. Then that old
  driver's shards 1-3 fail fast on purpose ($HOME/engine-asan is now a wrapper that exits 3 once asan-0.done exists).
- Shards 1-3 (--jobs 3, --mem-mb 1200, lead's call) by $HOME/logs/run-asan3.sh with $HOME/engine-asan-real, after
  asan-all.done; markers shard-k.done / shard-k.fail. Reports: $HOME/logs/asan/a.PID, $HOME/logs/ubsan/u.PID;
  `node $HOME/logs/triage.js` groups them by kind + top src/ frame.
- If the machine guard pauses/kills more than a handful of times in shard 1, back to --jobs 2.
- threadStackHigh thread_local duplicates builtins' ws/agent4 17f576c: on conflict take main's version.

- UBSan-only sweep queued: $HOME/logs/run-ubsan.sh (engine $HOME/engine-ubsan from $HOME/wt-ubsan,
  `-fsanitize=undefined`), starts after asan3-all.done + build-ubsan.done; markers ub-k.done/.fail, reports in
  $HOME/logs/ubsan2. Reason: GCC's combined ASan+UBSan prints UBSan reports to stderr (tools/lead/sanitizers.md).
- Notices are held after a notice-started turn (lead): the lead pings at ETAs; check markers when pinged.

## Findings so far (tools/lead/sanitizers.md has the table)

- UBSan: memcpy/memmove with a null data() for 0 bytes at 5 sites (typedarray.cpp InitializeTypedArrayFromTypedArray,
  %TypedArray%.prototype.set x2; arraybuffer.cpp slice, transfer): guarded.
- ASan + SNOWMAN_GC_STRESS=1: class.cpp ClassElements collected mid-ClassDefinitionEvaluation: Rooted (lead approved).

## State at 23:50 UTC

- MERGE REQUEST sent: exact sha 8d966b9 (on main c2fcc37). Gate vs results/5f3218f30{,-full}.json: 0 / 0 regressions,
  0 new statics; full run 53,599 / 53,616 files; JSONL results/8d966b9/full.jsonl (results branch 17a9aea).
  Evidence: the 16 class files 32/32, 0 ASan reports at GC_STRESS=1 (before: 16); UBSan TypedArray*/ArrayBuffer
  30 report files before, 0 after.
- RUNNING (left for a successor if this agent stops):
  - UBSan shard 3/4 with engine-ubsan2 (fixed): $HOME/logs/run-ub3.sh -> ub3.done/.fail, reports $HOME/logs/ubsan3
    (`DIRS=$HOME/logs/ubsan3 node $HOME/logs/triage.js`). Shards 0-2 found only the memcpy cluster (fixed).
  - Widened GC_STRESS=1 pass: $HOME/logs/run-gcs2.sh (engine-asan2 = 8d966b9, --jobs 2, private copy $HOME/t262-b),
    --shard 0/2 of each listed directory; per-directory lines in $HOME/logs/gcs2-progress.txt, reports in
    $HOME/logs/gcs2 (`DIRS=$HOME/logs/gcs2 node $HOME/logs/triage.js`), marker gcs2.done/.fail. ~17k scenarios,
    ~1.5-2 h at 2 jobs. Not yet covered: --shard 1/2 of the same directories.
- A new find: root it like class.cpp (Rooted<T> across safepoints), rebuild the ASan engine, rerun the reporting
  files at GC_STRESS=1, one normal gate, a small merge; or write the report + patch here.
- Removed sanitizer engines/worktrees when done: $HOME/wt-asan, $HOME/wt-ubsan, $HOME/t262-b, $HOME/engine-*.

## Next steps

- 4 shards, one at a time: `node t262/runner/run.mjs --engine <asan engine> --suite full --shard k/4 --jobs 2 --vm-mb 0
  --mem-mb 1500 --out $HOME/logs/asan-k.jsonl` with ASAN_OPTIONS=log_path=$HOME/logs/asan/a,
  UBSAN_OPTIONS=log_path=$HOME/logs/ubsan/u:print_stacktrace=1.
- Triage by top frame into tools/lead/sanitizers.md.
