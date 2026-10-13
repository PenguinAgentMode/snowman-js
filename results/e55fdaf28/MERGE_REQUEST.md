# Merge request: ws/chinese e55fdaf28

Chinese/dangi month tables for related ISO years 1900..2100 computed astronomically in the generator
(tools/temporal/astro.mjs, Meeus) instead of ICU 78.3's low-precision astronomer; ICU data outside that range.
Files: tools/temporal/{astro,gen-calendars,check-calendars}.mjs, src/builtins/temporal/calendar_{chinese,dangi}_data.inc,
notes/chinese.md. No C++ change; table format unchanged. Base: main 786b6f2 (rebased).

## Gate (one full-suite run on exactly e55fdaf28, 4 jobs, 2,334 s; this directory's full.jsonl)
- `node tools/lead/gate.mjs results/317949e37.json full.jsonl`: es-core 43196 passing files; regressions: 0; expected: 0; not run: 0
- `node tools/lead/gate.mjs results/317949e37-full.json full.jsonl`: full 53414 → 53427 passing of 53616; regressions: 0; expected: 0; not run: 0
- built-ins/Temporal 4,605 / 4,605; intl402/Temporal 2,022 / 2,029; intl402 3,358 / 3,382.
- +13 files vs 317949e37: the 9 targets (daysInYear/basic-chinese.js ×4, monthCode/chinese-calendar-dates.js ×5) and
  calendars-ref's 4 PlainMonthDay files (already on main via addd9a2). So +9 vs current main.
- `node tools/temporal/check-calendars.mjs --engine ./engine`: all ok (Meeus examples 4/4; byte-identical regeneration;
  table = Intl outside 1900..2100; 1900..2100 differs from Intl in exactly the listed years; engine = table).

## Years where astronomy and ICU differ (each a new moon or major solar term within ~15 min of midnight)
chinese 1906 1917 1922 1953 1954 1955 1987 1998 2012 2018 2026 2027 2029 2030 2057 2070; dangi 2017 2051 2096.
In test262's range (1969..2048) astronomy matches every expectation: chinese 1987 leap M06L (ICU M07L), 2026 354 days,
2027 354 (CNY 02-06, new moon 23:56:02 CST), 2029 355, 2030 354 (CNY 02-03, new moon 00:07 CST); 1998/2012/2018 differ
only in one month start and dangi 2017 likewise (not tested). True edges, decided by < 1 min: chinese 2057-09-28
23:59:57, dangi 2051-11-03 23:59:08, 2097-01-13 23:59:55. Chinese before 1929 uses Beijing local mean time
(UTC+7:45:40, as Dershowitz & Reingold), which keeps ICU's 1914/1916/1920 and moves 1906's M04 to 04-23. Dangi keeps
ICU's zones (UTC+8 to 1911, +9 from 1912); UTC+8:30 in 1954-61 changes no day (checked).
