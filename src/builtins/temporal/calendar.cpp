// Temporal calendars: identifiers, Calendar Fields Records (PrepareCalendarFields and friends) and the
// iso8601 calendar's operations (CalendarDateAdd, CalendarDateUntil, …).
#include <cmath>

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
  if (lower != u"iso8601") return ThrowRangeError("unknown calendar: " + utf16ToUtf8(id));
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
  (void)calendar;  // CalendarExtraFields is empty for iso8601
  uint32_t fieldNames = calendarFieldNames | nonCalendarFieldNames;
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
  (void)calendar;
  uint32_t additionalKeys = fieldKeysPresent(additionalFields);
  // CalendarFieldKeysToIgnore (iso8601): the keys themselves, and month ↔ monthCode.
  uint32_t overriddenKeys = additionalKeys;
  if (additionalKeys & (kFieldMonth | kFieldMonthCode)) overriddenKeys |= kFieldMonth | kFieldMonthCode;
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
  (void)calendar;
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
  UncheckedISODate result = TRY(RegulateISODate(*fields.year, *fields.month, *fields.day, overflow));
  return CheckISODateWithinLimits(result.year, result.month, result.day);
}

Result<ISODate> CalendarYearMonthFromFields(const CalendarId& calendar, CalendarFields& fields, Overflow overflow) {
  TRY(CalendarResolveFields(calendar, fields, FieldsType::YearMonth));
  fields.day = 1;
  UncheckedISODate result = TRY(RegulateISODate(*fields.year, *fields.month, 1, overflow));
  if (!ISOYearMonthWithinLimits(result.year, result.month)) return ThrowRangeError("year-month is outside the supported range");
  return ISODate{int32_t(result.year), result.month, 1};
}

Result<ISODate> CalendarMonthDayFromFields(const CalendarId& calendar, CalendarFields& fields, Overflow overflow) {
  TRY(CalendarResolveFields(calendar, fields, FieldsType::MonthDay));
  constexpr int32_t referenceISOYear = 1972;
  double year = fields.year ? *fields.year : referenceISOYear;
  UncheckedISODate result = TRY(RegulateISODate(year, *fields.month, *fields.day, overflow));
  return ISODate{referenceISOYear, result.month, result.day};
}

Result<ISODate> CalendarDateAdd(const CalendarId& calendar, const ISODate& isoDate, const DateDuration& duration, Overflow overflow) {
  (void)calendar;
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
  (void)calendar;
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
  (void)calendar;
  CalendarFields fields;
  if (type == FieldsType::Date || type == FieldsType::YearMonth) fields.year = isoDate.year;
  if (type == FieldsType::Date || type == FieldsType::MonthDay) fields.day = isoDate.day;
  fields.monthCode = ISOMonthCode(isoDate.month);
  return fields;
}

Result<ISODateTime> InterpretTemporalDateTimeFields(const CalendarId& calendar, CalendarFields& fields, Overflow overflow) {
  ISODate isoDate = TRY(CalendarDateFromFields(calendar, fields, overflow));
  TimeRecord time = TRY(RegulateTime(*fields.hour, *fields.minute, *fields.second, *fields.millisecond,
                                     *fields.microsecond, *fields.nanosecond, overflow));
  return CombineISODateAndTimeRecord(isoDate, time);
}

Value CalendarDateField(const CalendarId& calendar, const ISODate& d, std::u16string_view which) {
  (void)calendar;
  using builtins::num;
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
