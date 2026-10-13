// The non-ISO calendar systems (calendar_systems.h).
#include "builtins/temporal/calendar_systems.h"

#include "builtins/temporal/calendar_isolike.h"
#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

int32_t CalendarSystem::daysInYear(int64_t year) const {
  int32_t days = 0;
  for (int32_t m = 1, n = monthsInYear(year); m <= n; m++) days += daysInMonth(year, m);
  return days;
}

std::optional<int32_t> CalendarSystem::ordinalOf(int64_t year, MonthCodeParts code) const {
  if (code.leap || code.number < 1 || code.number > monthsInYear(year)) return std::nullopt;
  return code.number;
}

std::pair<const EraRow*, int64_t> CalendarSystem::eraOf(int64_t, const Date& date) const {
  // Calendars whose eras are an epoch era and a negative era before it.
  std::span<const EraRow> rows = eras();
  for (const EraRow& row : rows) {
    if (row.kind == EraRow::Kind::Epoch && date.year >= 1) return {&row, date.year};
  }
  for (const EraRow& row : rows) {
    if (row.kind == EraRow::Kind::Negative) return {&row, 1 - date.year};
  }
  return {&rows[0], date.year};
}

std::optional<MonthCodeParts> ParseMonthCode(std::u16string_view code) {
  if (code.size() != 3 && code.size() != 4) return std::nullopt;
  if (code[0] != u'M' || code[1] < u'0' || code[1] > u'9' || code[2] < u'0' || code[2] > u'9') return std::nullopt;
  if (code.size() == 4 && code[3] != u'L') return std::nullopt;
  int32_t number = (code[1] - u'0') * 10 + (code[2] - u'0');
  if (number == 0 && code.size() == 3) return std::nullopt;
  return MonthCodeParts{number, code.size() == 4};
}

std::u16string CreateMonthCode(MonthCodeParts parts) {
  std::u16string code = u"M";
  code += char16_t(u'0' + parts.number / 10);
  code += char16_t(u'0' + parts.number % 10);
  if (parts.leap) code += u'L';
  return code;
}

namespace {

const EraRow kGregoryEras[] = {
    {u"ce", u"ad", EraRow::Kind::Epoch, 0},
    {u"bce", u"bc", EraRow::Kind::Negative, 0},
};

const IsoLikeCalendar kGregory(0, kGregoryEras);

const EraRow kBuddhistEras[] = {
    {u"be", nullptr, EraRow::Kind::Epoch, 0},
};
// Buddhist Era year 1 is 543 BCE (ISO year -542).
const IsoLikeCalendar kBuddhist(-543, kBuddhistEras);

const EraRow kRocEras[] = {
    {u"roc", nullptr, EraRow::Kind::Epoch, 0},
    {u"broc", nullptr, EraRow::Kind::Negative, 0},
};
// Minguo year 1 is ISO 1912.
const IsoLikeCalendar kRoc(1911, kRocEras);

}  // namespace

const CalendarSystem* GetCalendarSystem(std::u16string_view calendar) {
  if (calendar == u"gregory") return &kGregory;
  if (calendar == u"buddhist") return &kBuddhist;
  if (calendar == u"roc") return &kRoc;
  if (calendar == u"japanese") return JapaneseCalendarSystem();
  if (calendar == u"chinese") return ChineseCalendarSystem();
  if (calendar == u"dangi") return DangiCalendarSystem();
  if (calendar == u"islamic-umalqura") return IslamicUmalquraCalendarSystem();
  return nullptr;
}

}  // namespace snowman::temporal
