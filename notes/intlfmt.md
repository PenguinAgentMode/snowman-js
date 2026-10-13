# intlfmt: handoff note

Owner: intlfmt (intl402 leftovers: Intl.DateTimeFormat in non-Gregorian calendars). Lead: the agent that started intlfmt.

## Branch / state

- Branch `ws/intlfmt`, based on main 53d4d86 (gate baseline 317949e37).
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

## Gaps (known, not tested by test262)

- iso8601 calendar uses Gregorian patterns (cldr-json has no ca-iso8601; ICU prints 2000-01-01).
- `_numbers` overrides (zh chinese d=hanidays, he y=hebr) are ignored: digits stay latn.

## Next steps

- full-suite gate in the background; gate.mjs vs results/317949e37{,-full}.json; push JSONL to results branch.

## Gotchas

- `cd tools/intl && npm install` (cldr-cal-*-full are devDependencies now) before re-running gen-date-data.mjs.
- Output must stay out of the conversation: date_data.inc is ~800 KB.
