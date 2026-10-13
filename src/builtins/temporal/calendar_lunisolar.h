// Table-driven lunisolar calendars (chinese, dangi): month lengths and leap months per year from a table generated
// from ICU (tools/temporal/gen-calendars.mjs), and a mean-lunation arithmetic outside the table's years.
#pragma once

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "builtins/temporal/calendar_systems.h"

namespace snowman::temporal {

// The years of a generated table: one packed uint32 per year (bits 0..12: the 0-based ordinal months with 30 days;
// bits 13..16: the 1-based ordinal of the leap month, 0 for none), the first year, and the epoch day of its month 1
// day 1. Each later year starts where the previous one ends.
struct LunisolarTable {
  int32_t firstYear;
  int64_t firstNewYear;
  std::span<const uint32_t> years;
};

class LunisolarTableCalendar final : public CalendarSystem {
 public:
  explicit LunisolarTableCalendar(LunisolarTable table) : table_(table) {
    starts_.reserve(table.years.size() + 1);
    int64_t start = table.firstNewYear;
    for (uint32_t packed : table.years) {
      starts_.push_back(start);
      start += yearLength(packed);
    }
    starts_.push_back(start);
    past_ = {table.firstYear, starts_.front()};
    future_ = {table.firstYear + int64_t(table.years.size()), starts_.back()};
  }

  Date fromEpochDays(int64_t epochDays) const override {
    if (epochDays >= starts_.front() && epochDays < starts_.back()) {
      size_t lo = 0, hi = table_.years.size() - 1;
      while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (starts_[mid] <= epochDays) lo = mid;
        else hi = mid - 1;
      }
      uint32_t packed = table_.years[lo];
      int32_t day = int32_t(epochDays - starts_[lo]);
      int32_t month = 1;
      for (; day >= monthLength(packed, month); month++) day -= monthLength(packed, month);
      return {table_.firstYear + int64_t(lo), month, day + 1};
    }
    const Arithmetic& a = arithmeticFor(epochDays < starts_.front());
    int64_t k = a.monthAt(epochDays);
    int64_t year = a.yearOfMonth(k);
    return {year, int32_t(k - a.cumulativeMonths(year) + 1), int32_t(epochDays - a.monthStart(k) + 1)};
  }

  int64_t toEpochDays(int64_t year, int32_t month, int32_t day) const override {
    if (const uint32_t* packed = row(year)) {
      int64_t days = starts_[size_t(year - table_.firstYear)];
      for (int32_t m = 1; m < month; m++) days += monthLength(*packed, m);
      return days + day - 1;
    }
    const Arithmetic& a = arithmeticFor(year < table_.firstYear);
    return a.monthStart(a.cumulativeMonths(year) + month - 1) + day - 1;
  }

  int32_t monthsInYear(int64_t year) const override {
    if (const uint32_t* packed = row(year)) return leapOrdinal(*packed) ? 13 : 12;
    const Arithmetic& a = arithmeticFor(year < table_.firstYear);
    return int32_t(a.cumulativeMonths(year + 1) - a.cumulativeMonths(year));
  }

  int32_t daysInMonth(int64_t year, int32_t month) const override {
    if (const uint32_t* packed = row(year)) return monthLength(*packed, month);
    const Arithmetic& a = arithmeticFor(year < table_.firstYear);
    int64_t k = a.cumulativeMonths(year) + month - 1;
    return int32_t(a.monthStart(k + 1) - a.monthStart(k));
  }

  bool inLeapYear(int64_t year) const override { return monthsInYear(year) == 13; }

  MonthCodeParts monthCodeOf(int64_t year, int32_t month) const override {
    int32_t leap = leapOrdinalOf(year);
    if (leap == 0 || month < leap) return {month, false};
    if (month == leap) return {month - 1, true};
    return {month - 1, false};
  }

  std::optional<int32_t> ordinalOf(int64_t year, MonthCodeParts code) const override {
    if (code.number < 1 || code.number > 12) return std::nullopt;
    int32_t leap = leapOrdinalOf(year);
    if (code.leap) {
      if (leap == 0 || leap - 1 != code.number) return std::nullopt;
      return leap;
    }
    return leap == 0 || code.number < leap ? code.number : code.number + 1;
  }

  // M01..M12 and their leap months M01L..M12L.
  bool isValidMonthCode(MonthCodeParts code) const override { return code.number >= 1 && code.number <= 12; }
  // A missing leap month constrains to the common month of the same number (M05L -> M05).
  LeapShift leapShift() const override { return LeapShift::SkipBackward; }
  int32_t maxDaysInMonthCode(MonthCodeParts) const override { return 30; }

 private:
  // Outside the table: mean lunations (29.530588853 days) from an anchor at the table's edge, and 235 months per
  // 19 years; a 13-month year's leap month is its 7th month (M06L). Consistent in both directions, not astronomy.
  struct Arithmetic {
    int64_t anchorYear;  // its month 1 starts at anchorDay
    int64_t anchorDay;
    static constexpr double kLunation = 29.530588853;
    static int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
    int64_t cumulativeMonths(int64_t year) const { return floorDiv((year - anchorYear) * 235, 19); }
    int64_t monthStart(int64_t k) const { return anchorDay + int64_t(std::floor(double(k) * kLunation)); }
    int64_t monthAt(int64_t epochDays) const {
      int64_t k = int64_t(std::floor(double(epochDays - anchorDay) / kLunation));
      while (monthStart(k) > epochDays) k--;
      while (monthStart(k + 1) <= epochDays) k++;
      return k;
    }
    int64_t yearOfMonth(int64_t k) const {
      int64_t year = anchorYear + floorDiv(k * 19, 235);
      while (cumulativeMonths(year) > k) year--;
      while (cumulativeMonths(year + 1) <= k) year++;
      return year;
    }
  };

  static int32_t leapOrdinal(uint32_t packed) { return int32_t((packed >> 13) & 15); }
  static int32_t monthLength(uint32_t packed, int32_t month) { return packed & (1u << (month - 1)) ? 30 : 29; }
  static int64_t yearLength(uint32_t packed) {
    int64_t days = 0;
    for (int32_t m = 1, n = leapOrdinal(packed) ? 13 : 12; m <= n; m++) days += monthLength(packed, m);
    return days;
  }

  const uint32_t* row(int64_t year) const {
    int64_t i = year - table_.firstYear;
    return i >= 0 && i < int64_t(table_.years.size()) ? &table_.years[size_t(i)] : nullptr;
  }
  int32_t leapOrdinalOf(int64_t year) const {
    if (const uint32_t* packed = row(year)) return leapOrdinal(*packed);
    return monthsInYear(year) == 13 ? 7 : 0;
  }
  const Arithmetic& arithmeticFor(bool past) const { return past ? past_ : future_; }

  LunisolarTable table_;
  std::vector<int64_t> starts_;  // starts_[i]: the epoch day of month 1 day 1 of firstYear + i; one past the end too
  Arithmetic past_{0, 0};    // years before the table, anchored at its first year
  Arithmetic future_{0, 0};  // years after it, anchored where it ends
};

}  // namespace snowman::temporal
