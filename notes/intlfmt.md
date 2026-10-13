# intlfmt: handoff note

Owner: intlfmt (intl402 leftovers: Intl.DateTimeFormat in non-Gregorian calendars). Lead: the agent that started intlfmt.

## Branch / state

- Branch `ws/intlfmt`, rebased on main 786b6f2 (STATUS/notes only since 53d4d86; gate baseline 317949e37).
- Gated: ac942bf2d (full suite, 4 jobs, 2,479 s). gate.mjs vs results/317949e37.json: 0 regressions; vs
  results/317949e37-full.json: 0 regressions. Full 53,441 / 53,616 (+27: 17 DateTimeFormat + 6 Ramadan dateStyle mine,
  4 PlainMonthDay/from from addd9a2, already on main); ES-core 43,196 (unchanged). JSONL: results branch
  results/ac942bf2d/full.jsonl. Merge request sent to the lead.
- Done (WIP, not yet gated): tools/intl/gen-date-data.mjs now also emits kCalendarFields (cldr-cal-*-full 48.0.0 +
  ca-generic from cldr-dates-full): months (hebrew 14 incl. 7-yeartype-leap), days, day periods, date/time/atTime
  formats, availableFormats, appendItems, intervalFormats, leap monthPatterns (31), cyclic year names (32-34),
  eras by Temporal era code (35-37). Lookup = locale-first, then calendar chain, then that locale's Gregorian field;
  the generator simulates it and writes only entries that change the result.
- datetimeformat.cpp: calendarField(locale, calendar, field) (memoized), Generator per (locale, calendar),
  CalendarFields via temporal::CalendarISOToDate; G (era codes; japanese bce/ce -> Gregorian eras), y (era year;
  chinese/dangi: cycle year), u, r (relatedYear part), U (yearName part), M/L (calendar month names, hebrew Adar
  I/II, chinese leap "{0}bis" format/stand-alone/numeric), d; range greatest difference on calendar fields.
- GetDateTimeFormat: `era` alone no longer counts as anyPresent (temporal-objects-format-with-era, 4 files).

## Numbers (slice, this branch)

- intl402/DateTimeFormat 245 / 245 (main 228). intl402 total 3,372 / 3,382 (main 317949e37: 3,345).
- Left: 9 Chinese-calendar Temporal files (known gap, Temporal tables vs HKO), Temporal/ZonedDateTime/links.js (tz data).

## Checks

- gen-date-data.mjs re-run is byte-identical. check-dtf.mjs (default 10 Gregorian locales, 3,470 cases): output
  identical to main's engine (458 differ from node on both, pre-existing). Calendar locales vs node (DTF_INSTANTS
  1886..2024): remaining diffs are ICU quirks (ja-japanese yy in Gy年 patterns, chinese G = cycle number, which
  test262's era.js forbids), pre-existing ko/fa Gregorian diffs, hanidays.
- links.js: node's tz 2026d keeps CST6CDT as its own Zone (1936/1960 -06:00 vs Chicago -05:00); data gap, left.

## Gaps (known, not tested by test262)

- iso8601 calendar uses Gregorian patterns (cldr-json has no ca-iso8601; ICU prints 2000-01-01).
- `_numbers` overrides (zh chinese d=hanidays, he y=hebr) are ignored: digits stay latn.

## Next steps

- After merge: optional, untested by test262: iso8601 patterns (y-MM-dd), `_numbers` overrides (hanidays, hebr).

## Gotchas

- `cd tools/intl && npm install` (cldr-cal-*-full are devDependencies now) before re-running gen-date-data.mjs.
- Output must stay out of the conversation: date_data.inc is ~800 KB.
