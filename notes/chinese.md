# chinese (workstream handoff)

Branch `ws/chinese`, base main 53d4d86. Task: the 9 intl402/Temporal Chinese-calendar files (STATUS "Known gaps").

## What changed
- `tools/temporal/astro.mjs` (new): Meeus, "Astronomical Algorithms" 2nd ed.: new moons (ch. 49, periodic + 14
  planetary terms), the Sun's apparent longitude (VSOP87 Earth L/R abridged as in Appendix III, FK5, 4-term
  nutation, aberration), ΔT (Espenak & Meeus 2006 polynomials). Reproduces Meeus 49.a and 25.b/32.a exactly.
- `tools/temporal/gen-calendars.mjs`: `ASTRONOMICAL` years 1900..2100 for chinese and dangi come from astronomy
  (month = local day of the new moon; month 11 contains the December solstice; in a 13-month sui the first month
  without a major solar term is leap); ICU data elsewhere. Zones: chinese UTC+7:45:40 before 1929, UTC+8 after
  (Dershowitz & Reingold); dangi UTC+8 to 1911, UTC+9 from 1912 (ICU's rule; +8:30 in 1954-61 changes no day,
  checked). The astronomical years start and end on ICU's days (asserted). stderr lists every year differing from
  ICU and every deciding event < 5 min from midnight. Table format unchanged; calendar_*.cpp untouched.
- `tools/temporal/check-calendars.mjs`: check 0 (Meeus examples); check 2 scans every day of 1900..2100 and requires
  the differences from ICU to be exactly `ICU_DEVIATIONS`; check 4 compares the engine with the table.
- Data: `calendar_{chinese,dangi}_data.inc` regenerated (umalqura byte-identical).

## Differences from ICU (all new moons/terms within ~15 min of midnight)
chinese 1906 1917 1922 1953 1954 1955 1987 1998 2012 2018 2026 2027 2029 2030 2057 2070; dangi 2017 2051 2096.
test262-covered ones agree with astronomy (1987 M06L; 2026 354, 2027 354/CNY 02-06, 2029 355, 2030 CNY 02-03).
True edges (< 1 min): chinese 2057-09-28 23:59:57, dangi 2051-11-03 23:59:08, 2097-01-13 23:59:55.

## Numbers
- intl402/Temporal slice (full suite filter): 4,044 / 4,058 scenarios; vs 317949e37: +26 scenarios (the 9 targets
  + calendars-ref's 4 files, which are on main), 0 regressions. Remaining 14: Ramadan dateStyle (6 files), links.js.
- check-calendars --engine: all ok.

## Next
- Full-suite gate on the exact sha, gate.mjs vs 317949e37 (ES-core + full), push JSONL to results/<sha>/full.jsonl.
