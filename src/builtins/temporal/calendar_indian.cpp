// The indian (Saka) calendar: the year starts on 22 March (21 March in a Gregorian leap year) of ISO year
// saka + 78; Chaitra has 30 days (31 in a leap year), the next five months 31, the last six 30 (ICU's arithmetic).
#include "builtins/temporal/calendar_arithmetic.h"
#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

namespace {

const EraRow kIndianEras[] = {{u"shaka", nullptr, EraRow::Kind::Epoch, 0}};

class IndianCalendar final : public YearStartCalendar {
 public:
  int32_t monthsInYear(int64_t) const override { return 12; }
  int32_t daysInMonth(int64_t year, int32_t month) const override {
    if (month == 1) return MathematicalInLeapYear(double(year + 78)) ? 31 : 30;
    return month <= 6 ? 31 : 30;
  }
  int32_t maxDaysInMonthCode(MonthCodeParts code) const override { return code.number <= 6 ? 31 : 30; }
  std::span<const EraRow> eras() const override { return kIndianEras; }

 protected:
  int64_t yearStart(int64_t year) const override {
    return ISODateToEpochDays(year + 78, 2, MathematicalInLeapYear(double(year + 78)) ? 21 : 22);
  }
  int64_t estimateYear(int64_t epochDays) const override { return EpochDaysToISODate(epochDays).year - 78; }
  int64_t minYearLength() const override { return 365; }
};

const IndianCalendar kIndian;

}  // namespace

const CalendarSystem* IndianCalendarSystem() { return &kIndian; }

}  // namespace snowman::temporal
