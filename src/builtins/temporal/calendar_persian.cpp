// The persian (Solar Hijri) calendar as ICU 78 computes it: the 33-year arithmetic rule (Farvardin 1 of year y is
// 365(y-1) + floor((8y+21)/33) days after the epoch), with ICU's list of years that are not leap years although
// the rule says so (the leap day moves to the next year). Months: six of 31 days, five of 30, Esfand 29 or 30.
#include "builtins/temporal/calendar_arithmetic.h"

#include <algorithm>
#include <iterator>

namespace snowman::temporal {

namespace {

// ICU 78 persncal.cpp, nonLeapYears.
constexpr int16_t kNonLeapYears[] = {
    1502, 1601, 1634, 1667, 1700, 1733, 1766, 1799, 1832, 1865, 1898, 1931, 1964, 1997, 2030, 2059,
    2063, 2096, 2129, 2158, 2162, 2191, 2195, 2224, 2228, 2257, 2261, 2290, 2294, 2323, 2327, 2356,
    2360, 2389, 2393, 2422, 2426, 2455, 2459, 2488, 2492, 2521, 2525, 2554, 2558, 2587, 2591, 2620,
    2624, 2653, 2657, 2686, 2690, 2719, 2723, 2748, 2752, 2756, 2781, 2785, 2789, 2818, 2822, 2847,
    2851, 2855, 2880, 2884, 2888, 2913, 2917, 2921, 2946, 2950, 2954, 2979, 2983, 2987,
};
bool isNonLeapYear(int64_t year) {
  return year >= kNonLeapYears[0] && year <= kNonLeapYears[std::size(kNonLeapYears) - 1] &&
         std::binary_search(std::begin(kNonLeapYears), std::end(kNonLeapYears), int16_t(year));
}

// The epoch day of Farvardin 1, AP 1 (ICU's PERSIAN_EPOCH, Julian day 1948320).
constexpr int64_t kPersianEpoch = -492268;

const EraRow kPersianEras[] = {{u"ap", nullptr, EraRow::Kind::Epoch, 0}};

class PersianCalendar final : public YearStartCalendar {
 public:
  int32_t monthsInYear(int64_t) const override { return 12; }
  int32_t daysInMonth(int64_t year, int32_t month) const override {
    if (month <= 6) return 31;
    if (month <= 11) return 30;
    return int32_t(yearStart(year + 1) - yearStart(year) - 336);
  }
  int32_t maxDaysInMonthCode(MonthCodeParts code) const override { return code.number <= 6 ? 31 : 30; }
  std::span<const EraRow> eras() const override { return kPersianEras; }

 protected:
  int64_t yearStart(int64_t year) const override {
    return kPersianEpoch + 365 * (year - 1) + FloorDiv(8 * year + 21, 33) - (isNonLeapYear(year - 1) ? 1 : 0);
  }
  int64_t estimateYear(int64_t epochDays) const override { return FloorDiv(33 * (epochDays - kPersianEpoch) + 3, 12053) + 1; }
  int64_t minYearLength() const override { return 365; }
};

const PersianCalendar kPersian;

}  // namespace

const CalendarSystem* PersianCalendarSystem() { return &kPersian; }

}  // namespace snowman::temporal
