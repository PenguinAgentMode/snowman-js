# survey (intl workstream): handoff note

Owner: survey (took over intl after intl's agent died of a context overflow). Lead: the agent that started survey.

## Branch / state

- **Branch: `ws/survey-intl` only** (lead, 2026-10-13: never push to ws/intl-dev or ws/intl; the old intl agent
  may still be alive on stale state. If ws/intl-dev moves to a sha that isn't ours, tell the lead and ignore it).
- DateTimeFormat batch (old ws/intl-dev b086e3d..ccdf619) rebased onto main 46746ad as faae9a2..80243d8. The
  rebase was clean: no file it changes was changed on main since its old base 797c7f7. 32879c42e adds this note.
- **Gated sha: 32879c42e**. Full suite, 4 jobs, 2,210 s; JSONL + gate output on the results branch,
  `results/32879c42e/`.
  - `gate.mjs results/c21bcd82e.json`: regressions 0.
  - `gate.mjs results/c21bcd82e-full.json`: regressions 0.
  - Branch run: 51,272 / 53,616 files (ES-core 43,166, the same as e535e4b's gate).
- Files outside `src/builtins/intl/`: `src/builtins/date.cpp` (toLocale* → `intl::dateToLocaleString`, own commit
  7a51505); `tools/intl/{check-dtf,gen-casing-data,gen-date-data,gen-locale-data}.mjs`, `tools/intl/package.json`.
  It reads `src/builtins/temporal/{temporal.h,time_zone.h}` read-only.

## Numbers (32879c42e vs main c21bcd82e)

- intl402 non-Temporal: 1,069 → 1,318 / 1,353. DateTimeFormat 3 → 210 / 245; String 15 → 19; Date 9 → 12;
  DurationFormat 105 → 110; Intl 63 → 66; top-level 2 → 22; FallbackSymbol 0 → 2; Collator, DisplayNames,
  Locale, NumberFormat, PluralRules +1 each (all now full).
- intl402/Temporal: 197 / 2,029 (unchanged: Temporal's toLocaleString isn't wired to DTF yet).
- The 3 waived Intl-gated staging files pass: sm/Proxy/revoked-get-function-realm-typeerror.js,
  sm/String/internalUsage.js, sm/extensions/quote-string-for-nul-character.js.

## Remaining intl402 non-Temporal: 35, all DateTimeFormat

- 25 have Temporal rejecting a calendar id ("unknown calendar: gregory" 22, chinese 1, buddhist 1, dangi 1):
  temporal's side (src/builtins/temporal/calendar*), coordinate via the lead.
- 10 are non-Gregorian DTF output: chinese/dangi relatedYear / yearName parts (5), pattern-on-calendar (2),
  islamic-civil eras (1), lunisolar leap months (1), related-year-zh (1).

## Next steps (lead's queue)

- (a) remaining DTF misses above (non-Gregorian; with temporal via the lead).
- (b) Temporal *.prototype.toLocaleString → DTF / DurationFormat (97 intl402/Temporal files). Touches temporal's
  `src/builtins/temporal/{duration,instant,plain_date,plain_date_time,plain_month_day,plain_time,
  plain_year_month,zoned_date_time}.cpp` (one toLocaleString body each) + `intl/hooks.h`. Separate small commit;
  tell the lead the files first.
- (c) the rest of intl402/Temporal.

## Gotchas

- Keep output out of the conversation: redirect build/run output to files, print summaries only.
- 4 GB machine: `make -j4`, `--jobs 4`; every background run gets a completion monitor.
- run.mjs now applies `ulimit -v` itself (`--vm-mb`, default 1024) and the 700 MB RSS watchdog: run `./engine`
  directly.
