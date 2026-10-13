# survey (intl workstream): handoff note

Owner: survey (took over intl after intl's agent died of a context overflow). Lead: the agent that started survey.

## Branch / state

- **Branch: `ws/survey-intl` only** (lead, 2026-10-13: never push to ws/intl-dev or ws/intl; the old intl agent
  may still be alive on stale state. If ws/intl-dev moves to a sha that is not ours, tell the lead and ignore it).
- Merged: 32879c42e (DTF batch) as main 68e8bb7; the Intl staging waivers were dropped.
- Current batch on main ec34f4f: a1452bb intl hooks for Temporal toLocaleString (CreateDateTimeFormat
  toLocaleStringTimeZone, CreateDurationFormat; an Instant formats with date+time defaults; zero offsets print
  gmtZeroFormat "GMT"), then 55400c3, the 8-file temporal commit approved by the lead (only the toLocaleString bodies + the
  hooks.h include in src/builtins/temporal/{duration,instant,plain_date,plain_date_time,plain_month_day,plain_time,
  plain_year_month,zoned_date_time}.cpp). Gate: pending, vs c21bcd82e (lead: until the next post-merge run).
- Slice (intl402, before the gate): non-Temporal 1,318 / 1,353 (unchanged); intl402/Temporal 197 → 254 / 2,029;
  */toLocaleString/ 67 / 97 (28 of the 30 left are "unknown calendar: gregory", 1 islamic month names, 1 Instant
  dateStyle Ramadan).

## Numbers of the merged DTF batch (32879c42e vs main c21bcd82e)

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
