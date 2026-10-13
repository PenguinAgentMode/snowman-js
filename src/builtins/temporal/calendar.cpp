// Temporal calendars: identifiers, Calendar Fields Records (PrepareCalendarFields and friends) and the
// iso8601 calendar's operations (CalendarDateAdd, CalendarDateUntil, …).
#include <cmath>

#include "builtins/temporal/calendar_systems.h"
#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

namespace {

std::u16string asciiLowercase(std::u16string_view s) {
  std::u16string out(s);
  for (char16_t& c : out)
    if (c >= u'A' && c <= u'Z') c = char16_t(c + 32);
  return out;
}

}  // namespace

Result<CalendarId> CanonicalizeCalendar(std::u16string_view id) {
  std::u16string lower = asciiLowercase(id);
  // CLDR aliases.
  if (lower == u"islamicc") lower = u"islamic-civil";
  if (lower == u"ethiopic-amete-alem") lower = u"ethioaa";
  if (lower != u"iso8601" && !GetCalendarSystem(lower)) return ThrowRangeError("unknown calendar: " + utf16ToUtf8(id));
  return lower;
}

Result<std::u16string> ParseTemporalCalendarString(std::u16string_view string) {
  Result<ParsedISODateTime> parsed = ParseISODateTime(string, kAllFormats);
  if (parsed.isNormal()) {
    const auto& calendar = parsed.value().calendar;
    return calendar ? *calendar : std::u16string(u"iso8601");
  }
  if (!IsAnnotationValueSyntax(string)) return ThrowRangeError("invalid calendar: " + utf16ToUtf8(string));
  return std::u16string(string);
}

const CalendarId* CalendarSlotOf(Value item) {
  if (!item.isObject()) return nullptr;
  Object* o = item.asObject();
  if (auto* d = o->as<PlainDateObject>()) return &d->calendar;
  if (auto* dt = o->as<PlainDateTimeObject>()) return &dt->calendar;
  if (auto* md = o->as<PlainMonthDayObject>()) return &md->calendar;
  if (auto* ym = o->as<PlainYearMonthObject>()) return &ym->calendar;
  if (auto* z = o->as<ZonedDateTimeObject>()) return &z->calendar;
  return nullptr;
}

Result<CalendarId> ToTemporalCalendarIdentifier(Value temporalCalendarLike) {
  if (const CalendarId* slot = CalendarSlotOf(temporalCalendarLike)) return *slot;
  if (!temporalCalendarLike.isString()) return ThrowTypeError("calendar must be a string");
  std::u16string identifier = TRY(ParseTemporalCalendarString(temporalCalendarLike.asString()->view()));
  return CanonicalizeCalendar(identifier);
}

Result<CalendarId> GetTemporalCalendarIdentifierWithISODefault(Object* item) {
  if (const CalendarId* slot = CalendarSlotOf(Value(item))) return *slot;
  Value calendarLike = TRY(Get(item, PropertyKey(u"calendar")));
  if (calendarLike.isUndefined()) return IsoCalendar();
  return ToTemporalCalendarIdentifier(calendarLike);
}

// ---------------------------------------------------------------------------------------------------------
// Non-ISO calendars (proposal-intl-era-monthcode), over calendar_systems.h

namespace {

using Sys = CalendarSystem;

const Sys* systemOf(const CalendarId& calendar) { return calendar == u"iso8601" ? nullptr : GetCalendarSystem(calendar); }

int64_t epochDaysOf(const ISODate& d) { return ISODateToEpochDays(d.year, d.month - 1, d.day); }

// Calendar years far outside the supported range (|ISO year| <= 275760) only arise transiently; beyond this bound
// arithmetic is cut short and the result is rejected by the ISO limits checks.
constexpr int64_t kYearBound = 1'000'000;

// CanonicalizeEraInCalendar: the era row, or nullptr.
const EraRow* CanonicalizeEraInCalendar(const Sys* sys, std::u16string_view era) {
  for (const EraRow& row : sys->eras()) {
    if (era == row.name || (row.alias && era == row.alias)) return &row;
  }
  return nullptr;
}

// CalendarDateArithmeticYearForEraYear
int64_t CalendarDateArithmeticYearForEraYear(const EraRow& row, int64_t eraYear) {
  switch (row.kind) {
    case EraRow::Kind::Epoch: return eraYear;
    case EraRow::Kind::Negative: return 1 - eraYear;
    case EraRow::Kind::Offset: return row.offset + eraYear - 1;
  }
  return eraYear;
}

// ConstrainMonthCode
Result<MonthCodeParts> ConstrainMonthCode(const Sys* sys, int64_t year, MonthCodeParts code, Overflow overflow) {
  if (sys->ordinalOf(year, code)) return code;
  if (overflow == Overflow::Reject) return ThrowRangeError("the month code does not exist in that year");
  if (sys->leapShift() == Sys::LeapShift::SkipForward) return MonthCodeParts{code.number + 1, false};
  return MonthCodeParts{code.number, false};
}

// BalanceNonISODate: months by years, then days through epoch days (the result may lie outside the ISO limits).
Sys::Date BalanceNonISODate(const Sys* sys, int64_t year, int64_t month, int64_t day) {
  while (month <= 0 && year > -kYearBound) {
    year--;
    month += sys->monthsInYear(year);
  }
  for (int32_t n = sys->monthsInYear(year); month > n && year < kYearBound; n = sys->monthsInYear(year)) {
    month -= n;
    year++;
  }
  if (year <= -kYearBound || year >= kYearBound) return {year, 1, 1};
  return sys->fromEpochDays(sys->toEpochDays(year, int32_t(month), 1) + day - 1);
}

// CalendarIntegersToISO
Result<ISODate> CalendarIntegersToISO(const Sys* sys, int64_t year, int64_t month, int64_t day) {
  if (year <= -kYearBound || year >= kYearBound || month < 1 || month > sys->monthsInYear(year) || day < 1 ||
      day > sys->daysInMonth(year, int32_t(month))) {
    return ThrowRangeError("date is outside the supported range");
  }
  return EpochDaysToISODate(sys->toEpochDays(year, int32_t(month), int32_t(day)));
}

// CompareSurpasses(sign, year, monthOrCode, day, target), with a month code or an ordinal month.
bool CompareSurpasses(int sign, int64_t year, std::optional<MonthCodeParts> code, int64_t month, int64_t day,
                      const Sys::Date& target, MonthCodeParts targetCode) {
  if (year != target.year) return sign * (year - target.year) > 0;
  if (code) {
    std::u16string a = CreateMonthCode(*code), b = CreateMonthCode(targetCode);
    if (a != b) return sign > 0 ? a > b : b > a;
  } else if (month != target.month) {
    return sign * (month - target.month) > 0;
  }
  if (day != target.day) return sign * (day - target.day) > 0;
  return false;
}

// NonISODateSurpasses(sign, from, to, years, months, weeks, days)
bool NonISODateSurpasses(const Sys* sys, int sign, const Sys::Date& parts, MonthCodeParts partsCode,
                         const Sys::Date& calDate2, MonthCodeParts code2, int64_t years, int64_t months, int64_t weeks,
                         int64_t days) {
  int64_t y0 = parts.year + years;
  if (CompareSurpasses(sign, y0, partsCode, 0, parts.day, calDate2, code2)) return true;
  MonthCodeParts constrained = ConstrainMonthCode(sys, y0, partsCode, Overflow::Constrain).release();
  int64_t m0 = *sys->ordinalOf(y0, constrained);
  Sys::Date monthsAdded = BalanceNonISODate(sys, y0, m0 + months, 1);
  if (CompareSurpasses(sign, monthsAdded.year, std::nullopt, monthsAdded.month, parts.day, calDate2, code2)) return true;
  if (weeks == 0 && days == 0) return false;
  int32_t daysInMonth = sys->daysInMonth(monthsAdded.year, monthsAdded.month);
  int64_t regulatedDay = std::min<int64_t>(parts.day, daysInMonth);
  Sys::Date balanced = BalanceNonISODate(sys, monthsAdded.year, monthsAdded.month, regulatedDay + 7 * weeks + days);
  return CompareSurpasses(sign, balanced.year, std::nullopt, balanced.month, balanced.day, calDate2, code2);
}

// NonISODateAdd
Result<ISODate> NonISODateAdd(const Sys* sys, const ISODate& isoDate, const DateDuration& duration, Overflow overflow) {
  Sys::Date parts = sys->fromEpochDays(epochDaysOf(isoDate));
  int64_t y0 = parts.year + duration.years;
  if (y0 <= -kYearBound || y0 >= kYearBound) return ThrowRangeError("date is outside the supported range");
  MonthCodeParts code = TRY(ConstrainMonthCode(sys, y0, sys->monthCodeOf(parts.year, parts.month), overflow));
  int64_t m0 = *sys->ordinalOf(y0, code);
  // endOfMonth = BalanceNonISODate(y0, m0 + months + 1, 0): the last day of the month `months` after m0.
  Sys::Date firstOfMonth = BalanceNonISODate(sys, y0, m0 + duration.months, 1);
  if (firstOfMonth.year <= -kYearBound || firstOfMonth.year >= kYearBound) {
    return ThrowRangeError("date is outside the supported range");
  }
  int32_t daysInMonth = sys->daysInMonth(firstOfMonth.year, firstOfMonth.month);
  int64_t regulatedDay = parts.day;
  if (regulatedDay > daysInMonth) {
    if (overflow == Overflow::Reject) return ThrowRangeError("the day does not exist in that month");
    regulatedDay = daysInMonth;
  }
  int64_t days = 7 * duration.weeks + duration.days;
  if (days < -1'000'000'000 || days > 1'000'000'000) return ThrowRangeError("date is outside the supported range");
  int64_t epochDays = sys->toEpochDays(firstOfMonth.year, firstOfMonth.month, int32_t(regulatedDay)) + days;
  if (epochDays < -100'000'001 || epochDays > 100'000'001) return ThrowRangeError("date is outside the supported range");
  ISODate result = EpochDaysToISODate(epochDays);
  if (!ISODateWithinLimits(result)) return ThrowRangeError("date is outside the supported range");
  return result;
}

// NonISODateUntil: the spec's one-unit-at-a-time loops, with the years and months loops started near their answer
// and the weeks and days taken from the epoch-day distance (what those loops count).
DateDuration NonISODateUntil(const Sys* sys, const ISODate& one, const ISODate& two, Unit largestUnit) {
  int sign = -CompareISODate(one, two);
  if (sign == 0) return DateDuration{};
  Sys::Date parts = sys->fromEpochDays(epochDaysOf(one));
  Sys::Date calDate2 = sys->fromEpochDays(epochDaysOf(two));
  MonthCodeParts partsCode = sys->monthCodeOf(parts.year, parts.month);
  MonthCodeParts code2 = sys->monthCodeOf(calDate2.year, calDate2.month);
  auto surpasses = [&](int64_t y, int64_t m, int64_t w, int64_t d) {
    return NonISODateSurpasses(sys, sign, parts, partsCode, calDate2, code2, y, m, w, d);
  };
  // The largest count (in sign's direction, starting from `start` steps of 1) for which surpasses is false.
  auto largest = [&](int64_t start, auto&& test) {
    int64_t count = start;
    while (count != 0 && test(count)) count -= sign;
    while (!test(count + sign)) count += sign;
    return count;
  };
  int64_t years = 0;
  if (largestUnit == Unit::Year) {
    int64_t estimate = calDate2.year - parts.year;
    years = largest(estimate, [&](int64_t y) { return surpasses(y, 0, 0, 0); });
  }
  int64_t months = 0;
  if (largestUnit == Unit::Year || largestUnit == Unit::Month) {
    int64_t estimate = 0;
    if (largestUnit == Unit::Month) {
      // Average month length of the calendar years in between.
      estimate = (epochDaysOf(two) - epochDaysOf(one)) * 12 / 366 - sign;
      if (sign * estimate < 0) estimate = 0;
    }
    months = largest(estimate, [&](int64_t m) { return surpasses(years, m, 0, 0); });
  }
  // The date `years` and `months` from `one` (day constrained), then whole weeks and days to `two`.
  int64_t y0 = parts.year + years;
  MonthCodeParts constrained = ConstrainMonthCode(sys, y0, partsCode, Overflow::Constrain).release();
  Sys::Date monthsAdded = BalanceNonISODate(sys, y0, *sys->ordinalOf(y0, constrained) + months, 1);
  int32_t regulatedDay = std::min<int32_t>(parts.day, sys->daysInMonth(monthsAdded.year, monthsAdded.month));
  int64_t days = epochDaysOf(two) - sys->toEpochDays(monthsAdded.year, monthsAdded.month, regulatedDay);
  int64_t weeks = 0;
  if (largestUnit == Unit::Week) {
    weeks = days / 7;
    days -= weeks * 7;
  }
  return DateDuration{years, months, weeks, days};
}

// The arithmetic year a Calendar Fields Record's year resolves to must stay where CalendarIntegersToISO works.
bool calendarYearInRange(double year) { return year > -double(kYearBound) && year < double(kYearBound); }

// NonISOCalendarDateToISO
Result<ISODate> NonISOCalendarDateToISO(const Sys* sys, CalendarFields& fields, Overflow overflow) {
  if (!calendarYearInRange(*fields.year)) return ThrowRangeError("date is outside the supported range");
  int64_t year = int64_t(*fields.year);
  if (fields.monthCode) TRY(ConstrainMonthCode(sys, year, *ParseMonthCode(*fields.monthCode), overflow));
  int32_t monthsInYear = sys->monthsInYear(year);
  int64_t month = int64_t(*fields.month);
  if (month > monthsInYear) {
    if (overflow == Overflow::Reject) return ThrowRangeError("the month does not exist in that year");
    month = monthsInYear;
  }
  int32_t daysInMonth = sys->daysInMonth(year, int32_t(month));
  int64_t day = int64_t(*fields.day);
  if (day > daysInMonth) {
    if (overflow == Overflow::Reject) return ThrowRangeError("the day does not exist in that month");
    day = daysInMonth;
  }
  return CalendarIntegersToISO(sys, year, month, day);
}

// NonISOMonthDayToISOReferenceDate
Result<ISODate> NonISOMonthDayToISOReferenceDate(const Sys* sys, CalendarFields& fields, Overflow overflow) {
  MonthCodeParts code;
  int32_t daysInMonth;
  if (fields.year) {
    // The year must be one CalendarIntegersToISO can place within the ISO limits.
    if (!calendarYearInRange(*fields.year)) return ThrowRangeError("year is outside the supported range");
    int64_t year = int64_t(*fields.year);
    ISODate first = TRY(CalendarIntegersToISO(sys, year, 1, 1));
    ISODate last = EpochDaysToISODate(sys->toEpochDays(year, sys->monthsInYear(year), 1));
    if (!ISODateWithinLimits(first) && !ISODateWithinLimits(last)) return ThrowRangeError("year is outside the supported range");
    int32_t monthsInYear = sys->monthsInYear(year);
    int64_t month = int64_t(*fields.month);
    if (month > monthsInYear) {
      if (overflow == Overflow::Reject) return ThrowRangeError("the month does not exist in that year");
      month = monthsInYear;
    }
    code = fields.monthCode ? TRY(ConstrainMonthCode(sys, year, *ParseMonthCode(*fields.monthCode), overflow))
                            : sys->monthCodeOf(year, int32_t(month));
    daysInMonth = sys->daysInMonth(year, int32_t(month));
  } else {
    code = *ParseMonthCode(*fields.monthCode);
    daysInMonth = sys->maxDaysInMonthCode(code);
  }
  int64_t day = int64_t(*fields.day);
  if (day > daysInMonth) {
    if (overflow == Overflow::Reject) return ThrowRangeError("the day does not exist in that month");
    day = daysInMonth;
  }
  // The latest ISO date from 1900-01-01 to 1972-12-31 with this month code and day, else the earliest from
  // 1973-01-01 to 2035-12-31.
  auto search = [&](int64_t fromDays, int64_t toDays, bool latest) -> std::optional<ISODate> {
    int64_t y1 = sys->fromEpochDays(fromDays).year, y2 = sys->fromEpochDays(toDays).year;
    for (int64_t i = 0; i <= y2 - y1; i++) {
      int64_t y = latest ? y2 - i : y1 + i;
      std::optional<int32_t> m = sys->ordinalOf(y, code);
      if (!m || sys->daysInMonth(y, *m) < day) continue;
      int64_t epochDays = sys->toEpochDays(y, *m, int32_t(day));
      if (epochDays >= fromDays && epochDays <= toDays) return EpochDaysToISODate(epochDays);
    }
    return std::nullopt;
  };
  if (auto d = search(ISODateToEpochDays(1900, 0, 1), ISODateToEpochDays(1972, 11, 31), true)) return *d;
  if (auto d = search(ISODateToEpochDays(1973, 0, 1), ISODateToEpochDays(2035, 11, 31), false)) return *d;
  return ThrowRangeError("no reference date for this month code and day");
}

// NonISOResolveFields
Result<void> NonISOResolveFields(const Sys* sys, CalendarFields& fields, FieldsType type) {
  bool supportsEra = !sys->eras().empty();
  bool needsYear = type == FieldsType::Date || type == FieldsType::YearMonth || !fields.monthCode || fields.month;
  bool needsDay = type == FieldsType::Date || type == FieldsType::MonthDay;
  if (needsYear && !fields.year) {
    if (!supportsEra) return ThrowTypeError("required property 'year' is missing or undefined");
    if (!fields.era || !fields.eraYear) return ThrowTypeError("required property 'year' (or 'era' and 'eraYear') is missing");
  }
  if (supportsEra) {
    if (fields.era && !fields.eraYear) return ThrowTypeError("'era' requires 'eraYear'");
    if (fields.eraYear && !fields.era) return ThrowTypeError("'eraYear' requires 'era'");
  }
  if (needsDay && !fields.day) return ThrowTypeError("required property 'day' is missing or undefined");
  if (!fields.month && !fields.monthCode) return ThrowTypeError("required property 'month' or 'monthCode' is missing or undefined");
  if (supportsEra && fields.eraYear) {
    const EraRow* era = CanonicalizeEraInCalendar(sys, *fields.era);
    if (!era) return ThrowRangeError("unknown era for this calendar: " + utf16ToUtf8(*fields.era));
    int64_t arithmeticYear = CalendarDateArithmeticYearForEraYear(*era, int64_t(*fields.eraYear));
    if (fields.year && *fields.year != double(arithmeticYear)) return ThrowRangeError("year and era/eraYear do not agree");
    fields.year = double(arithmeticYear);
  }
  fields.era.reset();
  fields.eraYear.reset();
  if (fields.monthCode) {
    std::optional<MonthCodeParts> code = ParseMonthCode(*fields.monthCode);
    if (!code || !sys->isValidMonthCode(*code)) return ThrowRangeError("invalid monthCode for this calendar");
    if (fields.year) {
      if (!calendarYearInRange(*fields.year)) return ThrowRangeError("year is outside the supported range");
      int64_t year = int64_t(*fields.year);
      MonthCodeParts constrained = sys->ordinalOf(year, *code) ? *code : ConstrainMonthCode(sys, year, *code, Overflow::Constrain).release();
      int32_t month = *sys->ordinalOf(year, constrained);
      if (fields.month && *fields.month != month) return ThrowRangeError("month and monthCode do not agree");
      fields.month = month;
    }
  }
  return {};
}

}  // namespace

CalendarDate CalendarISOToDate(const CalendarId& calendar, const ISODate& isoDate) {
  const Sys* sys = systemOf(calendar);
  if (!sys) return CalendarDate{std::nullopt, std::nullopt, isoDate.year, isoDate.month, ISOMonthCode(isoDate.month), isoDate.day};
  int64_t epochDays = epochDaysOf(isoDate);
  Sys::Date date = sys->fromEpochDays(epochDays);
  CalendarDate result{std::nullopt, std::nullopt, int32_t(date.year), date.month,
                      CreateMonthCode(sys->monthCodeOf(date.year, date.month)), date.day};
  if (!sys->eras().empty()) {
    auto [era, eraYear] = sys->eraOf(epochDays, date);
    result.era = std::u16string(era->name);
    result.eraYear = int32_t(eraYear);
  }
  return result;
}

// ---------------------------------------------------------------------------------------------------------
// Calendar Fields Records

namespace {

enum class Conversion { IntegerWithTruncation, PositiveIntegerWithTruncation, ToString, TimeZoneIdentifier, MonthCode, OffsetString };

struct FieldRow {
  const char16_t* property;
  FieldName name;
  Conversion conversion;
};

// Table 19, sorted by property name (code unit order).
const FieldRow kFieldRows[] = {
    {u"day", kFieldDay, Conversion::PositiveIntegerWithTruncation},
    {u"era", kFieldEra, Conversion::ToString},
    {u"eraYear", kFieldEraYear, Conversion::IntegerWithTruncation},
    {u"hour", kFieldHour, Conversion::IntegerWithTruncation},
    {u"microsecond", kFieldMicrosecond, Conversion::IntegerWithTruncation},
    {u"millisecond", kFieldMillisecond, Conversion::IntegerWithTruncation},
    {u"minute", kFieldMinute, Conversion::IntegerWithTruncation},
    {u"month", kFieldMonth, Conversion::PositiveIntegerWithTruncation},
    {u"monthCode", kFieldMonthCode, Conversion::MonthCode},
    {u"nanosecond", kFieldNanosecond, Conversion::IntegerWithTruncation},
    {u"offset", kFieldOffset, Conversion::OffsetString},
    {u"second", kFieldSecond, Conversion::IntegerWithTruncation},
    {u"timeZone", kFieldTimeZone, Conversion::TimeZoneIdentifier},
    {u"year", kFieldYear, Conversion::IntegerWithTruncation},
};

std::optional<double>* numericField(CalendarFields& f, FieldName name) {
  switch (name) {
    case kFieldEraYear: return &f.eraYear;
    case kFieldYear: return &f.year;
    case kFieldMonth: return &f.month;
    case kFieldDay: return &f.day;
    case kFieldHour: return &f.hour;
    case kFieldMinute: return &f.minute;
    case kFieldSecond: return &f.second;
    case kFieldMillisecond: return &f.millisecond;
    case kFieldMicrosecond: return &f.microsecond;
    case kFieldNanosecond: return &f.nanosecond;
    default: return nullptr;
  }
}

uint32_t fieldKeysPresent(const CalendarFields& f) {
  uint32_t keys = 0;
  if (f.era) keys |= kFieldEra;
  if (f.eraYear) keys |= kFieldEraYear;
  if (f.year) keys |= kFieldYear;
  if (f.month) keys |= kFieldMonth;
  if (f.monthCode) keys |= kFieldMonthCode;
  if (f.day) keys |= kFieldDay;
  if (f.hour) keys |= kFieldHour;
  if (f.minute) keys |= kFieldMinute;
  if (f.second) keys |= kFieldSecond;
  if (f.millisecond) keys |= kFieldMillisecond;
  if (f.microsecond) keys |= kFieldMicrosecond;
  if (f.nanosecond) keys |= kFieldNanosecond;
  if (f.offset) keys |= kFieldOffset;
  if (f.timeZone) keys |= kFieldTimeZone;
  return keys;
}

void copyField(CalendarFields& to, const CalendarFields& from, FieldName name) {
  switch (name) {
    case kFieldEra: to.era = from.era; break;
    case kFieldMonthCode: to.monthCode = from.monthCode; break;
    case kFieldOffset: to.offset = from.offset; break;
    case kFieldTimeZone: to.timeZone = from.timeZone; break;
    default: *numericField(to, name) = *numericField(const_cast<CalendarFields&>(from), name); break;
  }
}

}  // namespace

Result<CalendarFields> PrepareCalendarFields(const CalendarId& calendar, Object* fields, uint32_t calendarFieldNames,
                                             uint32_t nonCalendarFieldNames, std::optional<uint32_t> requiredFieldNames) {
  uint32_t fieldNames = calendarFieldNames | nonCalendarFieldNames;
  // CalendarExtraFields: era and eraYear alongside year in calendars with eras.
  const Sys* sys = systemOf(calendar);
  if (sys && !sys->eras().empty() && (calendarFieldNames & kFieldYear)) fieldNames |= kFieldEra | kFieldEraYear;
  CalendarFields result;
  bool any = false;
  for (const FieldRow& row : kFieldRows) {
    if (!(fieldNames & row.name)) continue;
    Value value = TRY(Get(fields, PropertyKey(row.property)));
    if (!value.isUndefined()) {
      any = true;
      switch (row.conversion) {
        case Conversion::IntegerWithTruncation: *numericField(result, row.name) = TRY(ToIntegerWithTruncation(value)); break;
        case Conversion::PositiveIntegerWithTruncation:
          *numericField(result, row.name) = TRY(ToPositiveIntegerWithTruncation(value));
          break;
        case Conversion::ToString: result.era = std::u16string(TRY(ToString(value))->view()); break;
        case Conversion::TimeZoneIdentifier: result.timeZone = TRY(ToTemporalTimeZoneIdentifier(value)); break;
        case Conversion::MonthCode: result.monthCode = TRY(ToMonthCode(value)); break;
        case Conversion::OffsetString: result.offset = TRY(ToOffsetString(value)); break;
      }
    } else if (requiredFieldNames) {
      if (*requiredFieldNames & row.name)
        return ThrowTypeError("required property '" + utf16ToUtf8(row.property) + "' is missing or undefined");
      if (row.name & kFieldsTime) *numericField(result, row.name) = 0;
    }
  }
  if (!requiredFieldNames && !any) return ThrowTypeError("object must have at least one recognized property");
  return result;
}

CalendarFields CalendarMergeFields(const CalendarId& calendar, const CalendarFields& fields, const CalendarFields& additionalFields) {
  uint32_t additionalKeys = fieldKeysPresent(additionalFields);
  // CalendarFieldKeysToIgnore: the keys themselves, month <-> monthCode, and (NonISOFieldKeysToIgnore) era, eraYear
  // and year together in calendars with eras, and era and eraYear for day/month changes with mid-year eras.
  uint32_t overriddenKeys = additionalKeys;
  if (additionalKeys & (kFieldMonth | kFieldMonthCode)) overriddenKeys |= kFieldMonth | kFieldMonthCode;
  if (const Sys* sys = systemOf(calendar); sys && !sys->eras().empty()) {
    if (additionalKeys & (kFieldEra | kFieldEraYear | kFieldYear)) overriddenKeys |= kFieldEra | kFieldEraYear | kFieldYear;
    if (sys->hasMidYearEras() && (additionalKeys & (kFieldDay | kFieldMonth | kFieldMonthCode)))
      overriddenKeys |= kFieldEra | kFieldEraYear;
  }
  CalendarFields merged;
  uint32_t fieldsKeys = fieldKeysPresent(fields);
  for (uint32_t bit = 1; bit <= kFieldTimeZone; bit <<= 1) {
    if ((fieldsKeys & bit) && !(overriddenKeys & bit)) copyField(merged, fields, FieldName(bit));
  }
  for (uint32_t bit = 1; bit <= kFieldTimeZone; bit <<= 1) {
    if (additionalKeys & bit) copyField(merged, additionalFields, FieldName(bit));
  }
  return merged;
}

Result<void> CalendarResolveFields(const CalendarId& calendar, CalendarFields& fields, FieldsType type) {
  if (const Sys* sys = systemOf(calendar)) return NonISOResolveFields(sys, fields, type);
  if ((type == FieldsType::Date || type == FieldsType::YearMonth) && !fields.year)
    return ThrowTypeError("required property 'year' is missing or undefined");
  if ((type == FieldsType::Date || type == FieldsType::MonthDay) && !fields.day)
    return ThrowTypeError("required property 'day' is missing or undefined");
  if (!fields.monthCode) {
    if (!fields.month) return ThrowTypeError("required property 'month' or 'monthCode' is missing or undefined");
    return {};
  }
  const std::u16string& monthCode = *fields.monthCode;
  // ISO 8601 has no leap months; months are M01..M12.
  if (monthCode.size() != 3) return ThrowRangeError("invalid monthCode for the iso8601 calendar");
  int32_t monthCodeInteger = (monthCode[1] - u'0') * 10 + (monthCode[2] - u'0');
  if (monthCodeInteger < 1 || monthCodeInteger > 12) return ThrowRangeError("invalid monthCode for the iso8601 calendar");
  if (fields.month && *fields.month != monthCodeInteger) return ThrowRangeError("month and monthCode do not agree");
  fields.month = monthCodeInteger;
  return {};
}

Result<ISODate> CalendarDateFromFields(const CalendarId& calendar, CalendarFields& fields, Overflow overflow) {
  TRY(CalendarResolveFields(calendar, fields, FieldsType::Date));
  if (const Sys* sys = systemOf(calendar)) {
    ISODate result = TRY(NonISOCalendarDateToISO(sys, fields, overflow));
    if (!ISODateWithinLimits(result)) return ThrowRangeError("date is outside the supported range");
    return result;
  }
  UncheckedISODate result = TRY(RegulateISODate(*fields.year, *fields.month, *fields.day, overflow));
  return CheckISODateWithinLimits(result.year, result.month, result.day);
}

Result<ISODate> CalendarYearMonthFromFields(const CalendarId& calendar, CalendarFields& fields, Overflow overflow) {
  TRY(CalendarResolveFields(calendar, fields, FieldsType::YearMonth));
  fields.day = 1;
  if (const Sys* sys = systemOf(calendar)) {
    ISODate result = TRY(NonISOCalendarDateToISO(sys, fields, overflow));
    if (!ISOYearMonthWithinLimits(result.year, result.month)) return ThrowRangeError("year-month is outside the supported range");
    return result;
  }
  UncheckedISODate result = TRY(RegulateISODate(*fields.year, *fields.month, 1, overflow));
  if (!ISOYearMonthWithinLimits(result.year, result.month)) return ThrowRangeError("year-month is outside the supported range");
  return ISODate{int32_t(result.year), result.month, 1};
}

Result<ISODate> CalendarMonthDayFromFields(const CalendarId& calendar, CalendarFields& fields, Overflow overflow) {
  TRY(CalendarResolveFields(calendar, fields, FieldsType::MonthDay));
  if (const Sys* sys = systemOf(calendar)) return NonISOMonthDayToISOReferenceDate(sys, fields, overflow);
  constexpr int32_t referenceISOYear = 1972;
  double year = fields.year ? *fields.year : referenceISOYear;
  UncheckedISODate result = TRY(RegulateISODate(year, *fields.month, *fields.day, overflow));
  return ISODate{referenceISOYear, result.month, result.day};
}

Result<ISODate> CalendarDateAdd(const CalendarId& calendar, const ISODate& isoDate, const DateDuration& duration, Overflow overflow) {
  if (const Sys* sys = systemOf(calendar)) return NonISODateAdd(sys, isoDate, duration, overflow);
  ISOYearMonth intermediate = BalanceISOYearMonth(int64_t(isoDate.year) + duration.years, int64_t(isoDate.month) + duration.months);
  if (intermediate.year < -300000 || intermediate.year > 300000) return ThrowRangeError("date is outside the supported range");
  UncheckedISODate regulated = TRY(RegulateISODate(double(intermediate.year), intermediate.month, isoDate.day, overflow));
  int64_t days = duration.days + 7 * duration.weeks;
  if (days < -1000000000 || days > 1000000000) return ThrowRangeError("date is outside the supported range");
  ISODate result = BalanceISODate(int64_t(regulated.year), regulated.month, int64_t(regulated.day) + days);
  if (!ISODateWithinLimits(result)) return ThrowRangeError("date is outside the supported range");
  return result;
}

namespace {

// ISODateSurpasses(sign, y1, m1, d1, isoDate2)
bool ISODateSurpasses(int sign, int64_t y1, int64_t m1, int64_t d1, const ISODate& two) {
  if (y1 != two.year) return sign * (y1 - two.year) > 0;
  if (m1 != two.month) return sign * (m1 - two.month) > 0;
  if (d1 != two.day) return sign * (d1 - two.day) > 0;
  return false;
}

}  // namespace

DateDuration CalendarDateUntil(const CalendarId& calendar, const ISODate& one, const ISODate& two, Unit largestUnit) {
  if (const Sys* sys = systemOf(calendar)) return NonISODateUntil(sys, one, two, largestUnit);
  int sign = -CompareISODate(one, two);
  if (sign == 0) return DateDuration{};
  int64_t years = 0;
  if (largestUnit == Unit::Year) {
    // The largest candidate years (in sign's direction) whose (y1 + years, m1, d1) does not surpass two.
    years = int64_t(two.year) - one.year;
    if (years != 0 && ISODateSurpasses(sign, int64_t(one.year) + years, one.month, one.day, two)) years -= sign;
  }
  int64_t months = 0;
  if (largestUnit == Unit::Year || largestUnit == Unit::Month) {
    int64_t total = (int64_t(two.year) - (int64_t(one.year) + years)) * 12 + (two.month - one.month);
    months = total;
    if (months != 0) {
      ISOYearMonth candidate = BalanceISOYearMonth(int64_t(one.year) + years, int64_t(one.month) + months);
      if (ISODateSurpasses(sign, candidate.year, candidate.month, one.day, two)) months -= sign;
    }
  }
  ISOYearMonth intermediate = BalanceISOYearMonth(int64_t(one.year) + years, int64_t(one.month) + months);
  int32_t day = std::min(int32_t(one.day), ISODaysInMonth(double(intermediate.year), intermediate.month));
  ISODate constrained{int32_t(intermediate.year), intermediate.month, day};
  int64_t days = ISODateToEpochDays(two.year, two.month - 1, two.day) -
                 ISODateToEpochDays(constrained.year, constrained.month - 1, constrained.day);
  int64_t weeks = 0;
  if (largestUnit == Unit::Week) {
    weeks = days / 7;
    days -= weeks * 7;
  }
  return DateDuration{years, months, weeks, days};
}

std::u16string ISOMonthCode(int32_t month) {
  std::u16string code = u"M";
  code += char16_t(u'0' + month / 10);
  code += char16_t(u'0' + month % 10);
  return code;
}

CalendarFields ISODateToFields(const CalendarId& calendar, const ISODate& isoDate, FieldsType type) {
  CalendarDate calendarDate = CalendarISOToDate(calendar, isoDate);
  CalendarFields fields;
  if (type == FieldsType::Date || type == FieldsType::YearMonth) fields.year = calendarDate.year;
  if (type == FieldsType::Date || type == FieldsType::MonthDay) fields.day = calendarDate.day;
  fields.monthCode = calendarDate.monthCode;
  return fields;
}

Result<ISODateTime> InterpretTemporalDateTimeFields(const CalendarId& calendar, CalendarFields& fields, Overflow overflow) {
  ISODate isoDate = TRY(CalendarDateFromFields(calendar, fields, overflow));
  TimeRecord time = TRY(RegulateTime(*fields.hour, *fields.minute, *fields.second, *fields.millisecond,
                                     *fields.microsecond, *fields.nanosecond, overflow));
  return CombineISODateAndTimeRecord(isoDate, time);
}

Value CalendarDateField(const CalendarId& calendar, const ISODate& d, std::u16string_view which) {
  using builtins::num;
  if (const Sys* sys = systemOf(calendar)) {
    CalendarDate date = CalendarISOToDate(calendar, d);
    if (which == u"era") return date.era ? Value(newString(*date.era)) : Value::undefined();
    if (which == u"eraYear") return date.eraYear ? num(*date.eraYear) : Value::undefined();
    if (which == u"year") return num(date.year);
    if (which == u"month") return num(date.month);
    if (which == u"monthCode") return Value(newString(date.monthCode));
    if (which == u"day") return num(date.day);
    if (which == u"dayOfWeek") return num(ISODayOfWeek(d));
    if (which == u"dayOfYear") return num(double(epochDaysOf(d) - sys->toEpochDays(date.year, 1, 1) + 1));
    if (which == u"weekOfYear" || which == u"yearOfWeek") return Value::undefined();
    if (which == u"daysInWeek") return num(7);
    if (which == u"daysInMonth") return num(sys->daysInMonth(date.year, date.month));
    if (which == u"daysInYear") return num(sys->daysInYear(date.year));
    if (which == u"monthsInYear") return num(sys->monthsInYear(date.year));
    if (which == u"inLeapYear") return Value::boolean(sys->inLeapYear(date.year));
    return Value::undefined();
  }
  if (which == u"era" || which == u"eraYear") return Value::undefined();
  if (which == u"year") return num(d.year);
  if (which == u"month") return num(d.month);
  if (which == u"monthCode") return Value(newString(ISOMonthCode(d.month)));
  if (which == u"day") return num(d.day);
  if (which == u"dayOfWeek") return num(ISODayOfWeek(d));
  if (which == u"dayOfYear") return num(ISODayOfYear(d));
  if (which == u"weekOfYear") return num(ISOWeekOfYear(d).week);
  if (which == u"yearOfWeek") return num(double(ISOWeekOfYear(d).year));
  if (which == u"daysInWeek") return num(7);
  if (which == u"daysInMonth") return num(ISODaysInMonth(d.year, d.month));
  if (which == u"daysInYear") return num(ISODaysInYear(d.year));
  if (which == u"monthsInYear") return num(12);
  if (which == u"inLeapYear") return Value::boolean(MathematicalInLeapYear(d.year));
  return Value::undefined();
}

}  // namespace snowman::temporal
