// Calendars with ISO 8601 months and days (gregory, buddhist, roc, japanese), for calendar_systems.cpp and
// calendar_japanese.cpp.
#pragma once

#include <span>

#include "builtins/temporal/calendar_systems.h"
#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

// Calendars with ISO 8601 months and days and an arithmetic year offset from the ISO year (gregory, buddhist,
// roc, japanese): arithmetic year = ISO year - epoch ISO year.
class IsoLikeCalendar : public CalendarSystem {
 public:
  IsoLikeCalendar(int64_t epochISOYear, std::span<const EraRow> eras) : epochISOYear_(epochISOYear), eras_(eras) {}

  Date fromEpochDays(int64_t epochDays) const override {
    ISODate d = EpochDaysToISODate(epochDays);
    return {d.year - epochISOYear_, d.month, d.day};
  }
  int64_t toEpochDays(int64_t year, int32_t month, int32_t day) const override {
    return ISODateToEpochDays(year + epochISOYear_, month - 1, day);
  }
  int32_t monthsInYear(int64_t) const override { return 12; }
  int32_t daysInMonth(int64_t year, int32_t month) const override {
    return ISODaysInMonth(double(year + epochISOYear_), month);
  }
  bool inLeapYear(int64_t year) const override { return MathematicalInLeapYear(double(year + epochISOYear_)); }
  int32_t maxDaysInMonthCode(MonthCodeParts code) const override { return ISODaysInMonth(1972, code.number); }
  std::span<const EraRow> eras() const override { return eras_; }

 private:
  int64_t epochISOYear_;
  std::span<const EraRow> eras_;
};

}  // namespace snowman::temporal
