// The coptic, ethiopic and ethioaa calendars: twelve 30-day months and a 13th of 5 days (6 in a leap year: the
// year before a Julian leap year, year % 4 == 3), from an epoch day (Calendrical Calculations; ICU's arithmetic).
//   coptic   : year 1 AM starts 284-08-29 (Julian); era "am" for every year.
//   ethiopic : year 1 AM (Amete Mihret) starts 8-08-29 (Julian); years <= 0 are era "aa" (Amete Alem, AA = AM + 5500).
//   ethioaa  : the Amete Alem year (AM + 5500), era "aa".
#include <cstdint>

#include "builtins/temporal/calendar_systems.h"

namespace snowman::temporal {

namespace {

int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
int64_t floorMod(int64_t a, int64_t b) { return a - b * floorDiv(a, b); }

class CopticFamilyCalendar final : public CalendarSystem {
 public:
  // epochDays: the epoch day of day 1 month 1 of AM year 1; yearOffset: arithmetic year = AM year + yearOffset.
  CopticFamilyCalendar(int64_t epochDays, int64_t yearOffset, std::span<const EraRow> eras, bool ethiopicEras)
      : epoch_(epochDays), yearOffset_(yearOffset), eras_(eras), ethiopicEras_(ethiopicEras) {}

  Date fromEpochDays(int64_t epochDays) const override {
    // The AM year: 1461 days per 4 years.
    int64_t am = floorDiv(4 * (epochDays - epoch_) + 1463, 1461);
    while (yearStart(am) > epochDays) am--;
    while (yearStart(am + 1) <= epochDays) am++;
    int64_t dayOfYear = epochDays - yearStart(am);
    return {am + yearOffset_, int32_t(dayOfYear / 30) + 1, int32_t(dayOfYear % 30) + 1};
  }
  int64_t toEpochDays(int64_t year, int32_t month, int32_t day) const override {
    return yearStart(year - yearOffset_) + 30 * int64_t(month - 1) + day - 1;
  }
  int32_t monthsInYear(int64_t) const override { return 13; }
  int32_t daysInMonth(int64_t year, int32_t month) const override {
    if (month < 13) return 30;
    return inLeapYear(year) ? 6 : 5;
  }
  bool inLeapYear(int64_t year) const override { return floorMod(year - yearOffset_, 4) == 3; }
  bool isValidMonthCode(MonthCodeParts code) const override { return !code.leap && code.number >= 1 && code.number <= 13; }
  std::optional<int32_t> ordinalOf(int64_t, MonthCodeParts code) const override {
    if (!isValidMonthCode(code)) return std::nullopt;
    return code.number;
  }
  int32_t maxDaysInMonthCode(MonthCodeParts code) const override { return code.number == 13 ? 6 : 30; }
  std::span<const EraRow> eras() const override { return eras_; }
  std::pair<const EraRow*, int64_t> eraOf(int64_t epochDays, const Date& date) const override {
    if (!ethiopicEras_) return {&eras_[0], date.year};
    // ethiopic: "am" from year 1, "aa" (offset -5499) before.
    if (date.year >= 1) return {&eras_[0], date.year};
    (void)epochDays;
    return {&eras_[1], date.year - eras_[1].offset + 1};
  }

 private:
  int64_t yearStart(int64_t am) const { return epoch_ + 365 * (am - 1) + floorDiv(am, 4); }

  int64_t epoch_;
  int64_t yearOffset_;
  std::span<const EraRow> eras_;
  bool ethiopicEras_;
};

// Epoch days (days since 1970-01-01): R.D. 103605 (284-08-29 Julian) and R.D. 2796 (8-08-29 Julian), minus 719163.
constexpr int64_t kCopticEpoch = 103605 - 719163;
constexpr int64_t kEthiopicEpoch = 2796 - 719163;

const EraRow kCopticEras[] = {{u"am", nullptr, EraRow::Kind::Epoch, 0}};
const EraRow kEthiopicEras[] = {
    {u"am", nullptr, EraRow::Kind::Epoch, 0},
    {u"aa", nullptr, EraRow::Kind::Offset, -5499},
};
const EraRow kEthioaaEras[] = {{u"aa", nullptr, EraRow::Kind::Epoch, 0}};

const CopticFamilyCalendar kCoptic(kCopticEpoch, 0, kCopticEras, false);
const CopticFamilyCalendar kEthiopic(kEthiopicEpoch, 0, kEthiopicEras, true);
const CopticFamilyCalendar kEthioaa(kEthiopicEpoch, 5500, kEthioaaEras, false);

}  // namespace

const CalendarSystem* CopticCalendarSystem() { return &kCoptic; }
const CalendarSystem* EthiopicCalendarSystem() { return &kEthiopic; }
const CalendarSystem* EthioaaCalendarSystem() { return &kEthioaa; }

}  // namespace snowman::temporal
