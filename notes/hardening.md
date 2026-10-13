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

## Next steps

- 4 shards, one at a time: `node t262/runner/run.mjs --engine <asan engine> --suite full --shard k/4 --jobs 2 --vm-mb 0
  --mem-mb 1500 --out $HOME/logs/asan-k.jsonl` with ASAN_OPTIONS=log_path=$HOME/logs/asan/a,
  UBSAN_OPTIONS=log_path=$HOME/logs/ubsan/u:print_stacktrace=1.
- Triage by top frame into tools/lead/sanitizers.md.
