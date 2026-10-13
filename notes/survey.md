# survey (intl workstream): handoff note

Owner: survey (took over intl after intl's agent died of a context overflow). Lead: the agent that started survey.

## Branch / state

- `ws/intl-dev`: DateTimeFormat batch (b086e3d..ccdf619) rebased onto main 46746ad: faae9a2..80243d8.
  The rebase was clean; no file it changes was changed on main since its old base 797c7f7.
- Files outside `src/builtins/intl/`: `src/builtins/date.cpp` (toLocale* hook, own commit 7a51505), and
  `tools/intl/{check-dtf,gen-casing-data,gen-date-data,gen-locale-data}.mjs` + `tools/intl/package.json`.
  It reads `src/builtins/temporal/{temporal.h,time_zone.h}` (temporal / temporal2's) read-only.
- Gate: pending (see below).

## Numbers (from intl's run on the old base, not yet re-measured)

- intl402 non-Temporal: main 1,069 / 1,353 → 1,317 / 1,353; DateTimeFormat 3 → 210 / 245.

## Next steps (lead's queue)

1. Gate the rebased tip: one full-suite run, `gate.mjs results/c21bcd82e.json` (ES-core) and
   `results/c21bcd82e-full.json` (full); the 3 waived Intl-gated staging files must pass.
2. Merge request to the lead.
3. (a) remaining DTF misses (non-Gregorian calendars; coordinate with temporal via the lead), (b) Temporal
   *.prototype.toLocaleString → DTF/DurationFormat (touches temporal's files: tell the lead first), (c) the rest.

## Gotchas

- Keep output out of the conversation: redirect build/run output to files, print summaries only.
- 4 GB machine: `make -j4`, `--jobs 4`; every background run gets a completion monitor.
