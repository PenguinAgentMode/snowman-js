# temporal2: handoff note

## Area 1: Temporal time zones, Instant, ZonedDateTime, Now (merged: main 9f1e7e5, ws/temporal2 99a40ae)

- Files: src/builtins/temporal/{time_zone.h,time_zone.cpp,tz_data.inc,instant.cpp,zoned_date_time.cpp,now.cpp},
  tools/temporal/{gen-tz.mjs,tz-names.txt,check-tz.mjs}.
- Numbers (main): built-ins/Temporal 4,605/4,605; intl402/Temporal 190/2,029 (rest: non-ISO calendars, Intl).
- tz data: `node tools/temporal/gen-tz.mjs` (node 22.23.3, ICU 78.3, tz 2026d, CLDR 48.0), byte-identical on re-run;
  `node tools/temporal/check-tz.mjs ./engine` checks the engine against node (0 mismatches).
- Gotcha: intl402 ZonedDateTime/links.js wants CST6CDT etc. == America/*; tzdb 2026d made them Zones again (data gap).

## Area 2: module proposals (branch ws/modules2, on main e535e4b)

- Commits: da9fc61 ImportModuleForRealm (ShadowRealm hook, interp/module.h); 86b00d8 parser (import defer * as ns,
  import source x, import.defer(), import.source(); ast::ImportPhase on ModuleRequest/ImportDeclaration/
  ImportExpression); 029f15b interp defer semantics + source-phase loading/linking; 556b136 text modules;
  a2e16a5 realm.h AbstractModuleSource intrinsics; 07778d2 builtins/abstract_module_source.{h,cpp}; 411d927
  $262.AbstractModuleSource; ca6b26b `<module source>` host module; b7df2b9 parser CoverInitializedName
  ([{a = 0}.x] = []); 87b1886 bytes modules (AllocateImmutableArrayBuffer); 3ca3118 parser B.3.2 labelled and
  duplicate block functions.
- Numbers: module-proposal files (features import-defer, source-phase-imports(+module-source), import-text,
  import-bytes; 516): 244 -> 515. Left: import-defer/evaluation-triggers/ignore-private-name-access.js (needs
  nonextensible-applies-to-private, interp/class.cpp). staging/sm destructuring 7/7; B.3.2 label + deprecated-redecl.
- Check loop: /tmp/modcheck.sh-style runs of test/language/{expressions/dynamic-import,import,module-code},
  test/built-ins/AbstractModuleSource, test/staging/source-phase-imports, diffed against results/<baseline>-full.json.
- Gotchas: the 242 negative-syntax proposal tests all still pass (import.defer/import.source must be called directly,
  never after `new`, no spread). import-defer-of-missing-module-fails.js passes for the right reason (the load
  rejects on ./missing.js before evaluation). The runner can't be killed from another tool call (PID namespace):
  let old runs finish.
- Not done (needs interp/function.cpp, not mine): staging block functions named `arguments`
  (block-scoped-functions-annex-b-arguments.js, regress-602621.js): B.3.2.1 must still assign the var binding when
  F is "arguments" (2d134b3 skips the candidate). Queue item (d): staging/sm/regress/regress-610026 (AST node size).

## Area 4 (current): Temporal non-ISO calendars (branch ws/calendars, from main 258ff44)

- Ownership: src/builtins/temporal/calendar*.{h,cpp} + the calendar parts of temporal.h (plus my tz files);
  plain_*.cpp/duration.cpp/parse.cpp/options.cpp only in small separate commits listed in the MR.
- Design: calendar_systems.{h,cpp}: one CalendarSystem per calendar (epoch days <-> year/ordinal month/day,
  months per year, month codes incl. leap months like M05L, eras). temporal.h: struct CalendarDate {era, eraYear,
  year, month, monthCode, day} + CalendarISOToDate(CalendarId, ISODate) (intl's DTF uses it; chinese/dangi: year =
  related ISO year, no era). calendar.cpp generic non-ISO paths: aliases (islamicc, ethiopic-amete-alem),
  CalendarExtraFields, CalendarFieldKeysToIgnore, CalendarResolveFields (TypeErrors before RangeErrors, era -> year),
  date -> ISO with overflow, PlainMonthDay reference date, NonISODateAdd/Until, ISODateToFields, getters
  (weekOfYear/yearOfWeek undefined for non-ISO). japanese: Meiji is an era only from 1873-01-01.
  tools/temporal/check-calendars.mjs vs node. Spec: /tmp/eramonth.txt from tc39/proposal-intl-era-monthcode.
- Order (each its own gated merge): (1) CalendarDate/CalendarISOToDate + gregory; (2) buddhist, roc, japanese;
  (3) coptic, ethiopic, ethioaa, indian, persian, hebrew, islamic-civil, islamic-tbla; (4) chinese, dangi,
  islamic-umalqura from tools/temporal/gen-calendars.mjs tables.
- Watch: survey's toLocaleString batch (ws/survey-intl 5aba3f349) lands soon: rebase onto it. builtins7's
  thread_local conversion touches temporal/time_zone.cpp (one cache).
- Spec text: `python3 /tmp/spec2txt2.py proposal-intl-era-monthcode/spec.emu` (tc39 repo); ops by line: era table +
  CalendarSupportsEra 97, CanonicalizeEraInCalendar 255, month codes 280-372, era year/arithmetic year + epoch ISO
  years 386-472, BalanceNonISODate 555, NonISODateSurpasses 596 (CompareSurpasses: temporal spec plaindate.html),
  NonISODateAdd 628, NonISODateUntil 655 (loops: compute days/weeks arithmetically), NonISOCalendarDateToISO 694,
  NonISOMonthDayToISOReferenceDate 724 (+ chinese/dangi reference-year table), CalendarExtraFields 843,
  NonISOFieldKeysToIgnore 855, NonISOResolveFields 875.
