// The hebrew calendar: the arithmetic calendar (molad of Tishri and the four postponement rules; Calendrical
// Calculations), as ICU. Leap years ((7y + 1) mod 19 < 7) have 13 months, the 6th being Adar I (month code M05L);
// Adar (Adar II in a leap year) is M06. Heshvan and Kislev vary with the year length. Era: am.
#include "builtins/temporal/calendar_arithmetic.h"

namespace snowman::temporal {

namespace {

// The epoch day of Tishri 1, AM 1 (R.D. -1373427).
constexpr int64_t kHebrewEpoch = -1373427 - 719163;

const EraRow kHebrewEras[] = {{u"am", nullptr, EraRow::Kind::Epoch, 0}};

bool hebrewLeapYear(int64_t year) { return FloorMod(7 * year + 1, 19) < 7; }

// Days from the epoch to Tishri 1 of `year`, before the two year-length postponements.
int64_t elapsedDays(int64_t year) {
  int64_t months = FloorDiv(235 * year - 234, 19);
  int64_t parts = 12084 + 13753 * months;
  int64_t days = 29 * months + FloorDiv(parts, 25920);
  if (FloorMod(3 * (days + 1), 7) < 3) days++;
  return days;
}

class HebrewCalendar final : public YearStartCalendar {
 public:
  int32_t monthsInYear(int64_t year) const override { return hebrewLeapYear(year) ? 13 : 12; }
  int32_t daysInMonth(int64_t year, int32_t month) const override {
    MonthCodeParts code = monthCodeOf(year, month);
    if (code.leap) return 30;  // Adar I
    switch (code.number) {
      case 2: return FloorMod(yearLength(year), 10) == 5 ? 30 : 29;  // Heshvan: long in a complete year
      case 3: return FloorMod(yearLength(year), 10) == 3 ? 29 : 30;  // Kislev: short in a deficient year
      case 1: case 5: case 7: case 9: case 11: return 30;
      default: return 29;
    }
  }
  bool inLeapYear(int64_t year) const override { return hebrewLeapYear(year); }

  MonthCodeParts monthCodeOf(int64_t year, int32_t month) const override {
    if (!hebrewLeapYear(year) || month < 6) return {month, false};
    if (month == 6) return {5, true};
    return {month - 1, false};
  }
  std::optional<int32_t> ordinalOf(int64_t year, MonthCodeParts code) const override {
    if (!isValidMonthCode(code)) return std::nullopt;
    bool leap = hebrewLeapYear(year);
    if (code.leap) return leap ? std::optional<int32_t>(6) : std::nullopt;
    return leap && code.number >= 6 ? code.number + 1 : code.number;
  }
  bool isValidMonthCode(MonthCodeParts code) const override {
    if (code.leap) return code.number == 5;
    return code.number >= 1 && code.number <= 12;
  }
  // A missing Adar I (M05L) constrains to Adar (M06).
  LeapShift leapShift() const override { return LeapShift::SkipForward; }
  int32_t maxDaysInMonthCode(MonthCodeParts code) const override {
    if (code.leap) return 30;
    return code.number <= 3 || code.number % 2 == 1 ? 30 : 29;
  }
  std::span<const EraRow> eras() const override { return kHebrewEras; }

 protected:
  int64_t yearStart(int64_t year) const override {
    int64_t ny0 = elapsedDays(year - 1), ny1 = elapsedDays(year), ny2 = elapsedDays(year + 1);
    int64_t correction = ny2 - ny1 == 356 ? 2 : ny1 - ny0 == 382 ? 1 : 0;
    return kHebrewEpoch + ny1 + correction;
  }
  int64_t estimateYear(int64_t epochDays) const override { return FloorDiv((epochDays - kHebrewEpoch) * 98496, 35975351) + 1; }
  int64_t minYearLength() const override { return 353; }

 private:
  int64_t yearLength(int64_t year) const { return yearStart(year + 1) - yearStart(year); }
};

const HebrewCalendar kHebrew;

}  // namespace

const CalendarSystem* HebrewCalendarSystem() { return &kHebrew; }

}  // namespace snowman::temporal
