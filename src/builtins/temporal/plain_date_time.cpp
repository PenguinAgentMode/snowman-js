// Temporal.PlainDateTime (proposal-temporal §5).
#include <cmath>

#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

using builtins::num;

Result<PlainDateTimeObject*> CreateTemporalDateTime(const ISODateTime& isoDateTime, const CalendarId& calendar, Object* newTarget) {
  if (!ISODateTimeWithinLimits(isoDateTime)) return ThrowRangeError("date-time is outside the supported range");
  if (!newTarget) newTarget = builtins::intrinsic(Intrinsic::TemporalPlainDateTime);
  ISODateTime dt = isoDateTime;
  dt.time.days = 0;
  return OrdinaryCreateFromConstructor<PlainDateTimeObject>(newTarget, Intrinsic::TemporalPlainDateTimePrototype, dt, calendar);
}

Result<PlainDateTimeObject*> ToTemporalDateTime(Value item, Value options) {
  ISODateTime result;
  CalendarId calendar;
  if (item.isObject()) {
    Object* o = item.asObject();
    if (auto* dt = o->as<PlainDateTimeObject>()) {
      TRY(GetTemporalOverflowOptionFrom(options));
      return CreateTemporalDateTime(dt->isoDateTime, dt->calendar);
    }
    if (auto* z = o->as<ZonedDateTimeObject>()) {
      ISODateTime isoDateTime = TRY(GetISODateTimeForZone(z->timeZone, z->epochNanoseconds));
      TRY(GetTemporalOverflowOptionFrom(options));
      return CreateTemporalDateTime(isoDateTime, z->calendar);
    }
    if (auto* d = o->as<PlainDateObject>()) {
      TRY(GetTemporalOverflowOptionFrom(options));
      return CreateTemporalDateTime(CombineISODateAndTimeRecord(d->isoDate, MidnightTimeRecord()), d->calendar);
    }
    calendar = TRY(GetTemporalCalendarIdentifierWithISODefault(o));
    CalendarFields fields = TRY(PrepareCalendarFields(calendar, o, kFieldsDate, kFieldsTime, 0u));
    Overflow overflow = TRY(GetTemporalOverflowOptionFrom(options));
    result = TRY(InterpretTemporalDateTimeFields(calendar, fields, overflow));
  } else {
    if (!item.isString()) return ThrowTypeError("a PlainDateTime must be a Temporal object, a property bag or a string");
    ParsedISODateTime parsed = TRY(ParseISODateTime(item.asString()->view(), kDateTime));
    TimeRecord time = parsed.time ? *parsed.time : MidnightTimeRecord();
    calendar = TRY(CanonicalizeCalendar(parsed.calendar ? *parsed.calendar : std::u16string(u"iso8601")));
    TRY(GetTemporalOverflowOptionFrom(options));
    result = CombineISODateAndTimeRecord(CreateISODateRecord(*parsed.year, parsed.month, parsed.day), time);
  }
  return CreateTemporalDateTime(result, calendar);
}

namespace {

Result<Value> PlainDateTime_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Temporal.PlainDateTime requires 'new'");
  double y = TRY(ToIntegerWithTruncation(call.args[0]));
  double mo = TRY(ToIntegerWithTruncation(call.args[1]));
  double d = TRY(ToIntegerWithTruncation(call.args[2]));
  double t[6] = {};
  for (size_t i = 0; i < 6; i++)
    if (!call.args[3 + i].isUndefined()) t[i] = TRY(ToIntegerWithTruncation(call.args[3 + i]));
  Value calendarValue = call.args[9];
  if (calendarValue.isUndefined()) calendarValue = builtins::str(u"iso8601");
  if (!calendarValue.isString()) return ThrowTypeError("calendar must be a string");
  CalendarId calendar = TRY(CanonicalizeCalendar(calendarValue.asString()->view()));
  if (!IsValidISODate(y, mo, d)) return ThrowRangeError("invalid date");
  if (!IsValidTime(t[0], t[1], t[2], t[3], t[4], t[5])) return ThrowRangeError("invalid time");
  if (std::fabs(y) > 300000) return ThrowRangeError("date-time is outside the supported range");
  ISODate isoDate = CreateISODateRecord(int32_t(y), int32_t(mo), int32_t(d));
  TimeRecord time = CreateTimeRecord(int32_t(t[0]), int32_t(t[1]), int32_t(t[2]), int32_t(t[3]), int32_t(t[4]), int32_t(t[5]));
  return Value(TRY(CreateTemporalDateTime(CombineISODateAndTimeRecord(isoDate, time), calendar, call.newTarget)));
}

Result<Value> PlainDateTime_from(NativeCall& call) { return Value(TRY(ToTemporalDateTime(call.args[0], call.args[1]))); }

Result<Value> PlainDateTime_compare(NativeCall& call) {
  PlainDateTimeObject* one = TRY(ToTemporalDateTime(call.args[0]));
  PlainDateTimeObject* two = TRY(ToTemporalDateTime(call.args[1]));
  return num(CompareISODateTime(one->isoDateTime, two->isoDateTime));
}

Result<PlainDateTimeObject*> thisDateTime(NativeCall& call, const char* method) {
  return thisTemporal<PlainDateTimeObject>(call.thisValue, method);
}

Result<Value> PlainDateTimePrototype_calendarId(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.calendarId"));
  return Value(newString(dt->calendar));
}

#define DATE_FIELD_GETTER(name)                                                                  \
  Result<Value> PlainDateTimePrototype_##name(NativeCall& call) {                               \
    PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype." #name)); \
    return CalendarDateField(dt->calendar, dt->isoDateTime.isoDate, u## #name);                 \
  }
DATE_FIELD_GETTER(era)
DATE_FIELD_GETTER(eraYear)
DATE_FIELD_GETTER(year)
DATE_FIELD_GETTER(month)
DATE_FIELD_GETTER(monthCode)
DATE_FIELD_GETTER(day)
DATE_FIELD_GETTER(dayOfWeek)
DATE_FIELD_GETTER(dayOfYear)
DATE_FIELD_GETTER(weekOfYear)
DATE_FIELD_GETTER(yearOfWeek)
DATE_FIELD_GETTER(daysInWeek)
DATE_FIELD_GETTER(daysInMonth)
DATE_FIELD_GETTER(daysInYear)
DATE_FIELD_GETTER(monthsInYear)
DATE_FIELD_GETTER(inLeapYear)
#undef DATE_FIELD_GETTER

#define TIME_GETTER(name)                                                                        \
  Result<Value> PlainDateTimePrototype_##name(NativeCall& call) {                               \
    PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype." #name)); \
    return num(dt->isoDateTime.time.name);                                                      \
  }
TIME_GETTER(hour)
TIME_GETTER(minute)
TIME_GETTER(second)
TIME_GETTER(millisecond)
TIME_GETTER(microsecond)
TIME_GETTER(nanosecond)
#undef TIME_GETTER

Result<Value> PlainDateTimePrototype_with(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.with"));
  if (!TRY(IsPartialTemporalObject(call.args[0]))) return ThrowTypeError("with() requires a partial date-time object");
  const CalendarId& calendar = dt->calendar;
  CalendarFields fields = ISODateToFields(calendar, dt->isoDateTime.isoDate, FieldsType::Date);
  const TimeRecord& t = dt->isoDateTime.time;
  fields.hour = t.hour;
  fields.minute = t.minute;
  fields.second = t.second;
  fields.millisecond = t.millisecond;
  fields.microsecond = t.microsecond;
  fields.nanosecond = t.nanosecond;
  CalendarFields partialDateTime =
      TRY(PrepareCalendarFields(calendar, call.args[0].asObject(), kFieldsDate, kFieldsTime, std::nullopt));
  fields = CalendarMergeFields(calendar, fields, partialDateTime);
  Overflow overflow = TRY(GetTemporalOverflowOptionFrom(call.args[1]));
  ISODateTime result = TRY(InterpretTemporalDateTimeFields(calendar, fields, overflow));
  return Value(TRY(CreateTemporalDateTime(result, calendar)));
}

Result<Value> PlainDateTimePrototype_withPlainTime(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.withPlainTime"));
  TimeRecord time = TRY(ToTimeRecordOrMidnight(call.args[0]));
  return Value(TRY(CreateTemporalDateTime(CombineISODateAndTimeRecord(dt->isoDateTime.isoDate, time), dt->calendar)));
}

Result<Value> PlainDateTimePrototype_withCalendar(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.withCalendar"));
  CalendarId calendar = TRY(ToTemporalCalendarIdentifier(call.args[0]));
  return Value(TRY(CreateTemporalDateTime(dt->isoDateTime, calendar)));
}

// AddDurationToDateTime(operation, dateTime, temporalDurationLike, options)
Result<Value> AddDurationToDateTime(ArithmeticOperation operation, PlainDateTimeObject* dateTime, Value temporalDurationLike,
                                    Value options) {
  DurationObject* duration = TRY(ToTemporalDuration(temporalDurationLike));
  if (operation == ArithmeticOperation::Subtract) duration = CreateNegatedTemporalDuration(duration);
  Overflow overflow = TRY(GetTemporalOverflowOptionFrom(options));
  InternalDuration internalDuration = ToInternalDurationRecordWith24HourDays(duration->fields);
  ISODateTime result = TRY(AddDateTime(dateTime->isoDateTime, dateTime->calendar, internalDuration, overflow));
  return Value(TRY(CreateTemporalDateTime(result, dateTime->calendar)));
}

Result<Value> PlainDateTimePrototype_add(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.add"));
  return AddDurationToDateTime(ArithmeticOperation::Add, dt, call.args[0], call.args[1]);
}

Result<Value> PlainDateTimePrototype_subtract(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.subtract"));
  return AddDurationToDateTime(ArithmeticOperation::Subtract, dt, call.args[0], call.args[1]);
}

// DifferenceTemporalPlainDateTime(operation, dateTime, other, options)
Result<Value> DifferenceTemporalPlainDateTime(DifferenceOperation operation, PlainDateTimeObject* dateTime, Value otherValue,
                                              Value options) {
  PlainDateTimeObject* other = TRY(ToTemporalDateTime(otherValue));
  if (!CalendarEquals(dateTime->calendar, other->calendar))
    return ThrowRangeError("cannot compute the difference between date-times of different calendars");
  Object* resolvedOptions = TRY(GetOptionsObject(options));
  DifferenceSettings settings =
      TRY(GetDifferenceSettings(operation, resolvedOptions, UnitGroup::DateTime, 0, Unit::Nanosecond, Unit::Day));
  if (CompareISODateTime(dateTime->isoDateTime, other->isoDateTime) == 0) return Value(TRY(CreateTemporalDuration(DurationFields{})));
  InternalDuration internalDuration = TRY(DifferencePlainDateTimeWithRounding(
      dateTime->isoDateTime, other->isoDateTime, dateTime->calendar, settings.largestUnit, settings.roundingIncrement,
      settings.smallestUnit, settings.roundingMode));
  DurationObject* result = TRY(TemporalDurationFromInternal(internalDuration, settings.largestUnit));
  if (operation == DifferenceOperation::Since) result = CreateNegatedTemporalDuration(result);
  return Value(result);
}

Result<Value> PlainDateTimePrototype_until(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.until"));
  return DifferenceTemporalPlainDateTime(DifferenceOperation::Until, dt, call.args[0], call.args[1]);
}

Result<Value> PlainDateTimePrototype_since(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.since"));
  return DifferenceTemporalPlainDateTime(DifferenceOperation::Since, dt, call.args[0], call.args[1]);
}

Result<Value> PlainDateTimePrototype_round(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.round"));
  Object* roundTo = TRY(GetRoundingOptionsArgument(call.args[0], u"smallestUnit", "Temporal.PlainDateTime.prototype.round"));
  int64_t roundingIncrement = TRY(GetRoundingIncrementOption(roundTo));
  RoundingMode roundingMode = TRY(GetRoundingModeOption(roundTo, RoundingMode::HalfExpand));
  Unit smallestUnit = TRY(GetTemporalUnitValuedOption(roundTo, u"smallestUnit", true));
  TRY(ValidateTemporalUnitValue(smallestUnit, UnitGroup::Time, false, Unit::Day));
  int64_t maximum;
  bool inclusive;
  if (smallestUnit == Unit::Day) {
    maximum = 1;
    inclusive = true;
  } else {
    maximum = MaximumTemporalDurationRoundingIncrement(smallestUnit);
    inclusive = false;
  }
  TRY(ValidateTemporalRoundingIncrement(roundingIncrement, maximum, inclusive));
  if (smallestUnit == Unit::Nanosecond && roundingIncrement == 1)
    return Value(TRY(CreateTemporalDateTime(dt->isoDateTime, dt->calendar)));
  ISODateTime result = RoundISODateTime(dt->isoDateTime, roundingIncrement, smallestUnit, roundingMode);
  return Value(TRY(CreateTemporalDateTime(result, dt->calendar)));
}

Result<Value> PlainDateTimePrototype_equals(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.equals"));
  PlainDateTimeObject* other = TRY(ToTemporalDateTime(call.args[0]));
  if (CompareISODateTime(dt->isoDateTime, other->isoDateTime) != 0) return Value::boolean(false);
  return Value::boolean(CalendarEquals(dt->calendar, other->calendar));
}

Result<Value> PlainDateTimePrototype_toString(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.toString"));
  Object* resolvedOptions = TRY(GetOptionsObject(call.args[0]));
  ShowCalendar showCalendar = TRY(GetTemporalShowCalendarNameOption(resolvedOptions));
  int digits = TRY(GetTemporalFractionalSecondDigitsOption(resolvedOptions));
  RoundingMode roundingMode = TRY(GetRoundingModeOption(resolvedOptions, RoundingMode::Trunc));
  Unit smallestUnit = TRY(GetTemporalUnitValuedOption(resolvedOptions, u"smallestUnit"));
  TRY(ValidateTemporalUnitValue(smallestUnit, UnitGroup::Time));
  if (smallestUnit == Unit::Hour) return ThrowRangeError("smallestUnit must be minutes or smaller");
  SecondsStringPrecision precision = TRY(ToSecondsStringPrecisionRecord(smallestUnit, digits));
  ISODateTime result = RoundISODateTime(dt->isoDateTime, precision.increment, precision.unit, roundingMode);
  if (!ISODateTimeWithinLimits(result)) return ThrowRangeError("rounded date-time is outside the supported range");
  return Value(newString(ISODateTimeToString(result, dt->calendar, precision.precision, showCalendar)));
}

Result<Value> PlainDateTimePrototype_toLocaleString(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.toLocaleString"));
  return Value(newString(ISODateTimeToString(dt->isoDateTime, dt->calendar, -1, ShowCalendar::Auto)));
}

Result<Value> PlainDateTimePrototype_toJSON(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.toJSON"));
  return Value(newString(ISODateTimeToString(dt->isoDateTime, dt->calendar, -1, ShowCalendar::Auto)));
}

Result<Value> PlainDateTimePrototype_valueOf(NativeCall&) {
  return ThrowTypeError("Temporal.PlainDateTime.prototype.valueOf: use compare() or equals() to compare date-times");
}

Result<Value> PlainDateTimePrototype_toZonedDateTime(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.toZonedDateTime"));
  TimeZone timeZone = TRY(ToTemporalTimeZoneIdentifier(call.args[0]));
  Object* resolvedOptions = TRY(GetOptionsObject(call.args[1]));
  Disambiguation disambiguation = TRY(GetTemporalDisambiguationOption(resolvedOptions));
  EpochNanoseconds epochNs = TRY(GetEpochNanosecondsForZone(timeZone, dt->isoDateTime, disambiguation));
  return Value(TRY(CreateTemporalZonedDateTime(epochNs, timeZone, dt->calendar)));
}

Result<Value> PlainDateTimePrototype_toPlainDate(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.toPlainDate"));
  return Value(TRY(CreateTemporalDate(dt->isoDateTime.isoDate, dt->calendar)));
}

Result<Value> PlainDateTimePrototype_toPlainTime(NativeCall& call) {
  PlainDateTimeObject* dt = TRY(thisDateTime(call, "Temporal.PlainDateTime.prototype.toPlainTime"));
  return Value(TRY(CreateTemporalTime(dt->isoDateTime.time)));
}

void installPlainDateTime(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor =
      defineConstructor(realm, Intrinsic::TemporalPlainDateTime, PlainDateTime_construct, 3, PropertyKey(u"PlainDateTime"),
                        proto, Intrinsic::TemporalPlainDateTimePrototype, false);
  defineValue(TemporalNamespace(realm), PropertyKey(u"PlainDateTime"), Value(ctor));
  defineFunction(realm, ctor, PropertyKey(u"compare"), 2, PlainDateTime_compare);
  defineFunction(realm, ctor, PropertyKey(u"from"), 1, PlainDateTime_from);

  struct Getter {
    const char16_t* name;
    NativeFn fn;
  };
  const Getter getters[] = {
      {u"calendarId", PlainDateTimePrototype_calendarId},     {u"era", PlainDateTimePrototype_era},
      {u"eraYear", PlainDateTimePrototype_eraYear},           {u"year", PlainDateTimePrototype_year},
      {u"month", PlainDateTimePrototype_month},               {u"monthCode", PlainDateTimePrototype_monthCode},
      {u"day", PlainDateTimePrototype_day},                   {u"hour", PlainDateTimePrototype_hour},
      {u"minute", PlainDateTimePrototype_minute},             {u"second", PlainDateTimePrototype_second},
      {u"millisecond", PlainDateTimePrototype_millisecond},   {u"microsecond", PlainDateTimePrototype_microsecond},
      {u"nanosecond", PlainDateTimePrototype_nanosecond},     {u"dayOfWeek", PlainDateTimePrototype_dayOfWeek},
      {u"dayOfYear", PlainDateTimePrototype_dayOfYear},       {u"weekOfYear", PlainDateTimePrototype_weekOfYear},
      {u"yearOfWeek", PlainDateTimePrototype_yearOfWeek},     {u"daysInWeek", PlainDateTimePrototype_daysInWeek},
      {u"daysInMonth", PlainDateTimePrototype_daysInMonth},   {u"daysInYear", PlainDateTimePrototype_daysInYear},
      {u"monthsInYear", PlainDateTimePrototype_monthsInYear}, {u"inLeapYear", PlainDateTimePrototype_inLeapYear},
  };
  for (const Getter& g : getters) defineAccessor(realm, proto, PropertyKey(g.name), g.fn, nullptr);
  defineFunction(realm, proto, PropertyKey(u"with"), 1, PlainDateTimePrototype_with);
  defineFunction(realm, proto, PropertyKey(u"withPlainTime"), 0, PlainDateTimePrototype_withPlainTime);
  defineFunction(realm, proto, PropertyKey(u"withCalendar"), 1, PlainDateTimePrototype_withCalendar);
  defineFunction(realm, proto, PropertyKey(u"add"), 1, PlainDateTimePrototype_add);
  defineFunction(realm, proto, PropertyKey(u"subtract"), 1, PlainDateTimePrototype_subtract);
  defineFunction(realm, proto, PropertyKey(u"until"), 1, PlainDateTimePrototype_until);
  defineFunction(realm, proto, PropertyKey(u"since"), 1, PlainDateTimePrototype_since);
  defineFunction(realm, proto, PropertyKey(u"round"), 1, PlainDateTimePrototype_round);
  defineFunction(realm, proto, PropertyKey(u"equals"), 1, PlainDateTimePrototype_equals);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, PlainDateTimePrototype_toString);
  defineFunction(realm, proto, PropertyKey(u"toLocaleString"), 0, PlainDateTimePrototype_toLocaleString);
  defineFunction(realm, proto, PropertyKey(u"toJSON"), 0, PlainDateTimePrototype_toJSON);
  defineFunction(realm, proto, PropertyKey(u"valueOf"), 0, PlainDateTimePrototype_valueOf);
  defineFunction(realm, proto, PropertyKey(u"toZonedDateTime"), 1, PlainDateTimePrototype_toZonedDateTime);
  defineFunction(realm, proto, PropertyKey(u"toPlainDate"), 0, PlainDateTimePrototype_toPlainDate);
  defineFunction(realm, proto, PropertyKey(u"toPlainTime"), 0, PlainDateTimePrototype_toPlainTime);
  defineValue(proto, PropertyKey(builtins::symbols().toStringTag), builtins::str(u"Temporal.PlainDateTime"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(TemporalPlainDateTime, kOrderDefault, installPlainDateTime);

}  // namespace snowman::temporal
