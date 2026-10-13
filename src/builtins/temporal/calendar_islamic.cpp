// The tabular islamic calendars: islamic-civil (epoch Friday 622-07-16 Julian) and islamic-tbla (Thursday
// 622-07-15). Months alternate 30 and 29 days; Dhu al-Hijjah has 30 in the 11 leap years of each 30-year cycle
// ((14 + 11y) mod 30 < 11). Eras: ah from year 1, bh before.
#include "builtins/temporal/calendar_arithmetic.h"

namespace snowman::temporal {

namespace {

const EraRow kIslamicEras[] = {
    {u"ah", nullptr, EraRow::Kind::Epoch, 0},
    {u"bh", nullptr, EraRow::Kind::Negative, 0},
};

class TabularIslamicCalendar final : public YearStartCalendar {
 public:
  explicit TabularIslamicCalendar(int64_t epochDays) : epoch_(epochDays) {}

  int32_t monthsInYear(int64_t) const override { return 12; }
  int32_t daysInMonth(int64_t year, int32_t month) const override {
    if (month == 12) return FloorMod(14 + 11 * year, 30) < 11 ? 30 : 29;
    return month % 2 == 1 ? 30 : 29;
  }
  int32_t maxDaysInMonthCode(MonthCodeParts code) const override { return code.number % 2 == 1 || code.number == 12 ? 30 : 29; }
  std::span<const EraRow> eras() const override { return kIslamicEras; }

 protected:
  int64_t yearStart(int64_t year) const override { return epoch_ + (year - 1) * 354 + FloorDiv(3 + 11 * year, 30); }
  int64_t estimateYear(int64_t epochDays) const override { return FloorDiv(30 * (epochDays - epoch_) + 10646, 10631); }
  int64_t minYearLength() const override { return 354; }

 private:
  int64_t epoch_;
};

const TabularIslamicCalendar kIslamicCivil(-492148);
const TabularIslamicCalendar kIslamicTbla(-492149);

}  // namespace

const CalendarSystem* IslamicCivilCalendarSystem() { return &kIslamicCivil; }
const CalendarSystem* IslamicTblaCalendarSystem() { return &kIslamicTbla; }

}  // namespace snowman::temporal
