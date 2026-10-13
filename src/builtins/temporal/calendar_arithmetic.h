// Shared helpers for the arithmetic calendars (calendar_indian.cpp, calendar_persian.cpp, calendar_islamic.cpp,
// calendar_hebrew.cpp): a calendar is its year starts (epoch days) and month lengths; dates follow from them.
#pragma once

#include <cstdint>

#include "builtins/temporal/calendar_systems.h"

namespace snowman::temporal {

inline int64_t FloorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
inline int64_t FloorMod(int64_t a, int64_t b) { return a - b * FloorDiv(a, b); }

// A calendar defined by yearStart(year) and daysInMonth(year, month); estimateYear(epochDays) may be off by a few years.
class YearStartCalendar : public CalendarSystem {
 public:
  Date fromEpochDays(int64_t epochDays) const override {
    int64_t year = estimateYear(epochDays);
    while (yearStart(year) > epochDays) year--;
    while (yearStart(year + 1) <= epochDays) year++;
    int32_t day = int32_t(epochDays - yearStart(year));
    int32_t month = 1;
    for (int32_t n = monthsInYear(year); month < n && day >= daysInMonth(year, month); month++) day -= daysInMonth(year, month);
    return {year, month, day + 1};
  }
  int64_t toEpochDays(int64_t year, int32_t month, int32_t day) const override {
    int64_t days = yearStart(year);
    for (int32_t m = 1; m < month; m++) days += daysInMonth(year, m);
    return days + day - 1;
  }
  bool inLeapYear(int64_t year) const override { return yearStart(year + 1) - yearStart(year) > minYearLength(); }

 protected:
  virtual int64_t yearStart(int64_t year) const = 0;
  virtual int64_t estimateYear(int64_t epochDays) const = 0;
  virtual int64_t minYearLength() const = 0;
};

}  // namespace snowman::temporal
