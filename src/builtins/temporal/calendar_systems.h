// The arithmetic of the non-ISO calendars (proposal-intl-era-monthcode): one CalendarSystem per calendar type,
// converting between epoch days and calendar dates (arithmetic year, ordinal month, day), with month codes
// (leap months like M05L) and eras. calendar.cpp builds the generic NonISO* operations on top.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace snowman::temporal {

// A month code's parts: "M05L" is {5, true}.
struct MonthCodeParts {
  int32_t number;
  bool leap;
};

// One row of the era table: the era (and its alias) and how its era years map to arithmetic years.
struct EraRow {
  enum class Kind : uint8_t { Epoch, Negative, Offset };
  const char16_t* name;
  const char16_t* alias;  // nullptr if none
  Kind kind;
  int64_t offset;  // Offset: the arithmetic year of era year 1
};

class CalendarSystem {
 public:
  virtual ~CalendarSystem() = default;

  struct Date {
    int64_t year;   // arithmetic year
    int32_t month;  // 1-based ordinal month
    int32_t day;
  };
  virtual Date fromEpochDays(int64_t epochDays) const = 0;
  // A valid (year, month, day).
  virtual int64_t toEpochDays(int64_t year, int32_t month, int32_t day) const = 0;
  virtual int32_t monthsInYear(int64_t year) const = 0;
  virtual int32_t daysInMonth(int64_t year, int32_t month) const = 0;
  virtual bool inLeapYear(int64_t year) const = 0;
  int32_t daysInYear(int64_t year) const;

  // Month codes. Calendars without leap months: month code = ordinal month.
  virtual MonthCodeParts monthCodeOf(int64_t /*year*/, int32_t month) const { return {month, false}; }
  // The ordinal month of `code` in `year`, nullopt if the year does not contain it.
  virtual std::optional<int32_t> ordinalOf(int64_t year, MonthCodeParts code) const;
  // IsValidMonthCodeForCalendar: M01..M12, plus the calendar's additional codes (M13, leap months).
  virtual bool isValidMonthCode(MonthCodeParts code) const { return !code.leap && code.number >= 1 && code.number <= 12; }
  // ConstrainMonthCode's "Leap to Common Month Transformation" for a leap month missing from a year.
  enum class LeapShift : uint8_t { None, SkipBackward, SkipForward };
  virtual LeapShift leapShift() const { return LeapShift::None; }
  // The most days the month described by `code` has in any year (PlainMonthDay without a year).
  virtual int32_t maxDaysInMonthCode(MonthCodeParts code) const = 0;

  // Eras (empty: the calendar has none).
  virtual std::span<const EraRow> eras() const { return {}; }
  // The era row and era year of the date at `epochDays` (calendars with eras).
  virtual std::pair<const EraRow*, int64_t> eraOf(int64_t epochDays, const Date& date) const;
  virtual bool hasMidYearEras() const { return false; }
};

// The calendar system of a canonical non-ISO calendar type, nullptr if there is none (iso8601, unsupported).
const CalendarSystem* GetCalendarSystem(std::u16string_view calendar);

// The table-driven calendars (calendar_chinese.cpp, calendar_dangi.cpp, calendar_islamic_umalqura.cpp).
const CalendarSystem* ChineseCalendarSystem();
const CalendarSystem* DangiCalendarSystem();
const CalendarSystem* IslamicUmalquraCalendarSystem();
const CalendarSystem* JapaneseCalendarSystem();  // calendar_japanese.cpp
// The arithmetic calendars (calendar_coptic.cpp, calendar_indian.cpp, calendar_persian.cpp, calendar_islamic.cpp,
// calendar_hebrew.cpp).
const CalendarSystem* CopticCalendarSystem();
const CalendarSystem* EthiopicCalendarSystem();
const CalendarSystem* EthioaaCalendarSystem();
const CalendarSystem* IndianCalendarSystem();
const CalendarSystem* PersianCalendarSystem();
const CalendarSystem* IslamicCivilCalendarSystem();
const CalendarSystem* IslamicTblaCalendarSystem();
const CalendarSystem* HebrewCalendarSystem();

// ParseMonthCode / CreateMonthCode.
std::optional<MonthCodeParts> ParseMonthCode(std::u16string_view monthCode);
std::u16string CreateMonthCode(MonthCodeParts parts);

}  // namespace snowman::temporal
