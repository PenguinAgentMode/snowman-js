// Temporal.PlainDate (proposal-temporal §3).
#include <cmath>

#include "builtins/intl/hooks.h"
#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

using builtins::num;

Result<PlainDateObject*> CreateTemporalDate(const ISODate& isoDate, const CalendarId& calendar, Object* newTarget) {
  if (!ISODateWithinLimits(isoDate)) return ThrowRangeError("date is outside the supported range");
  if (!newTarget) newTarget = builtins::intrinsic(Intrinsic::TemporalPlainDate);
  return OrdinaryCreateFromConstructor<PlainDateObject>(newTarget, Intrinsic::TemporalPlainDatePrototype, isoDate, calendar);
}

Result<PlainDateObject*> ToTemporalDate(Value item, Value options) {
  if (item.isObject()) {
    Object* o = item.asObject();
    if (auto* d = o->as<PlainDateObject>()) {
      TRY(GetTemporalOverflowOptionFrom(options));
      return CreateTemporalDate(d->isoDate, d->calendar);
    }
    if (auto* z = o->as<ZonedDateTimeObject>()) {
      ISODateTime isoDateTime = TRY(GetISODateTimeForZone(z->timeZone, z->epochNanoseconds));
      TRY(GetTemporalOverflowOptionFrom(options));
      return CreateTemporalDate(isoDateTime.isoDate, z->calendar);
    }
    if (auto* dt = o->as<PlainDateTimeObject>()) {
      TRY(GetTemporalOverflowOptionFrom(options));
      return CreateTemporalDate(dt->isoDateTime.isoDate, dt->calendar);
    }
    CalendarId calendar = TRY(GetTemporalCalendarIdentifierWithISODefault(o));
    CalendarFields fields = TRY(PrepareCalendarFields(calendar, o, kFieldsDate, 0, 0u));
    Overflow overflow = TRY(GetTemporalOverflowOptionFrom(options));
    ISODate isoDate = TRY(CalendarDateFromFields(calendar, fields, overflow));
    return CreateTemporalDate(isoDate, calendar);
  }
  if (!item.isString()) return ThrowTypeError("a PlainDate must be a Temporal object, a property bag or a string");
  ParsedISODateTime result = TRY(ParseISODateTime(item.asString()->view(), kDateTime));
  CalendarId calendar = TRY(CanonicalizeCalendar(result.calendar ? *result.calendar : std::u16string(u"iso8601")));
  TRY(GetTemporalOverflowOptionFrom(options));
  return CreateTemporalDate(CreateISODateRecord(*result.year, result.month, result.day), calendar);
}

Result<TimeRecord> ToTimeRecordOrMidnight(Value item) {
  if (item.isUndefined()) return MidnightTimeRecord();
  PlainTimeObject* plainTime = TRY(ToTemporalTime(item));
  return plainTime->time;
}

namespace {

Result<Value> PlainDate_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Temporal.PlainDate requires 'new'");
  double y = TRY(ToIntegerWithTruncation(call.args[0]));
  double m = TRY(ToIntegerWithTruncation(call.args[1]));
  double d = TRY(ToIntegerWithTruncation(call.args[2]));
  Value calendarValue = call.args[3];
  if (calendarValue.isUndefined()) calendarValue = builtins::str(u"iso8601");
  if (!calendarValue.isString()) return ThrowTypeError("calendar must be a string");
  CalendarId calendar = TRY(CanonicalizeCalendar(calendarValue.asString()->view()));
  if (!IsValidISODate(y, m, d)) return ThrowRangeError("invalid date");
  if (!ISODateWithinLimits(y, int32_t(m), int32_t(d))) return ThrowRangeError("date is outside the supported range");
  return Value(TRY(CreateTemporalDate(CreateISODateRecord(int32_t(y), int32_t(m), int32_t(d)), calendar, call.newTarget)));
}

Result<Value> PlainDate_from(NativeCall& call) { return Value(TRY(ToTemporalDate(call.args[0], call.args[1]))); }

Result<Value> PlainDate_compare(NativeCall& call) {
  PlainDateObject* one = TRY(ToTemporalDate(call.args[0]));
  PlainDateObject* two = TRY(ToTemporalDate(call.args[1]));
  return num(CompareISODate(one->isoDate, two->isoDate));
}

Result<PlainDateObject*> thisDate(NativeCall& call, const char* method) {
  return thisTemporal<PlainDateObject>(call.thisValue, method);
}

Result<Value> PlainDatePrototype_calendarId(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.calendarId"));
  return Value(newString(d->calendar));
}

#define DATE_FIELD_GETTER(name)                                                      \
  Result<Value> PlainDatePrototype_##name(NativeCall& call) {                       \
    PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype." #name)); \
    return CalendarDateField(d->calendar, d->isoDate, u## #name);                   \
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

Result<Value> PlainDatePrototype_toPlainYearMonth(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.toPlainYearMonth"));
  CalendarFields fields = ISODateToFields(d->calendar, d->isoDate, FieldsType::Date);
  ISODate isoDate = TRY(CalendarYearMonthFromFields(d->calendar, fields, Overflow::Constrain));
  return Value(TRY(CreateTemporalYearMonth(isoDate, d->calendar)));
}

Result<Value> PlainDatePrototype_toPlainMonthDay(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.toPlainMonthDay"));
  CalendarFields fields = ISODateToFields(d->calendar, d->isoDate, FieldsType::Date);
  ISODate isoDate = TRY(CalendarMonthDayFromFields(d->calendar, fields, Overflow::Constrain));
  return Value(TRY(CreateTemporalMonthDay(isoDate, d->calendar)));
}

// AddDurationToDate(operation, temporalDate, temporalDurationLike, options)
Result<Value> AddDurationToDate(ArithmeticOperation operation, PlainDateObject* temporalDate, Value temporalDurationLike,
                                Value options) {
  const CalendarId& calendar = temporalDate->calendar;
  DurationObject* duration = TRY(ToTemporalDuration(temporalDurationLike));
  if (operation == ArithmeticOperation::Subtract) duration = CreateNegatedTemporalDuration(duration);
  DateDuration dateDuration = ToDateDurationRecordWithoutTime(duration->fields);
  Overflow overflow = TRY(GetTemporalOverflowOptionFrom(options));
  ISODate result = TRY(CalendarDateAdd(calendar, temporalDate->isoDate, dateDuration, overflow));
  return Value(TRY(CreateTemporalDate(result, calendar)));
}

Result<Value> PlainDatePrototype_add(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.add"));
  return AddDurationToDate(ArithmeticOperation::Add, d, call.args[0], call.args[1]);
}

Result<Value> PlainDatePrototype_subtract(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.subtract"));
  return AddDurationToDate(ArithmeticOperation::Subtract, d, call.args[0], call.args[1]);
}

Result<Value> PlainDatePrototype_with(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.with"));
  if (!TRY(IsPartialTemporalObject(call.args[0]))) return ThrowTypeError("with() requires a partial date object");
  const CalendarId& calendar = d->calendar;
  CalendarFields fields = ISODateToFields(calendar, d->isoDate, FieldsType::Date);
  CalendarFields partialDate = TRY(PrepareCalendarFields(calendar, call.args[0].asObject(), kFieldsDate, 0, std::nullopt));
  fields = CalendarMergeFields(calendar, fields, partialDate);
  Overflow overflow = TRY(GetTemporalOverflowOptionFrom(call.args[1]));
  ISODate isoDate = TRY(CalendarDateFromFields(calendar, fields, overflow));
  return Value(TRY(CreateTemporalDate(isoDate, calendar)));
}

Result<Value> PlainDatePrototype_withCalendar(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.withCalendar"));
  CalendarId calendar = TRY(ToTemporalCalendarIdentifier(call.args[0]));
  return Value(TRY(CreateTemporalDate(d->isoDate, calendar)));
}

// DifferenceTemporalPlainDate(operation, temporalDate, other, options)
Result<Value> DifferenceTemporalPlainDate(DifferenceOperation operation, PlainDateObject* temporalDate, Value otherValue,
                                          Value options) {
  PlainDateObject* other = TRY(ToTemporalDate(otherValue));
  if (!CalendarEquals(temporalDate->calendar, other->calendar)) return ThrowRangeError("cannot compute the difference between dates of different calendars");
  Object* resolvedOptions = TRY(GetOptionsObject(options));
  DifferenceSettings settings = TRY(GetDifferenceSettings(operation, resolvedOptions, UnitGroup::Date, 0, Unit::Day, Unit::Day));
  if (CompareISODate(temporalDate->isoDate, other->isoDate) == 0) return Value(TRY(CreateTemporalDuration(DurationFields{})));
  DateDuration dateDifference = CalendarDateUntil(temporalDate->calendar, temporalDate->isoDate, other->isoDate, settings.largestUnit);
  InternalDuration duration = CombineDateAndTimeDuration(dateDifference, 0);
  if (settings.smallestUnit != Unit::Day || settings.roundingIncrement != 1) {
    ISODateTime isoDateTime = CombineISODateAndTimeRecord(temporalDate->isoDate, MidnightTimeRecord());
    ISODateTime isoDateTimeOther = CombineISODateAndTimeRecord(other->isoDate, MidnightTimeRecord());
    EpochNanoseconds originEpochNs = GetUTCEpochNanoseconds(isoDateTime);
    EpochNanoseconds destEpochNs = GetUTCEpochNanoseconds(isoDateTimeOther);
    duration = TRY(RoundRelativeDuration(duration, originEpochNs, destEpochNs, isoDateTime, nullptr, temporalDate->calendar,
                                         settings.largestUnit, settings.roundingIncrement, settings.smallestUnit,
                                         settings.roundingMode));
  }
  DurationObject* result = TRY(TemporalDurationFromInternal(duration, Unit::Day));
  if (operation == DifferenceOperation::Since) result = CreateNegatedTemporalDuration(result);
  return Value(result);
}

Result<Value> PlainDatePrototype_until(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.until"));
  return DifferenceTemporalPlainDate(DifferenceOperation::Until, d, call.args[0], call.args[1]);
}

Result<Value> PlainDatePrototype_since(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.since"));
  return DifferenceTemporalPlainDate(DifferenceOperation::Since, d, call.args[0], call.args[1]);
}

Result<Value> PlainDatePrototype_equals(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.equals"));
  PlainDateObject* other = TRY(ToTemporalDate(call.args[0]));
  if (CompareISODate(d->isoDate, other->isoDate) != 0) return Value::boolean(false);
  return Value::boolean(CalendarEquals(d->calendar, other->calendar));
}

Result<Value> PlainDatePrototype_toPlainDateTime(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.toPlainDateTime"));
  TimeRecord time = TRY(ToTimeRecordOrMidnight(call.args[0]));
  return Value(TRY(CreateTemporalDateTime(CombineISODateAndTimeRecord(d->isoDate, time), d->calendar)));
}

Result<Value> PlainDatePrototype_toZonedDateTime(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.toZonedDateTime"));
  Value item = call.args[0];
  TimeZone timeZone;
  Value temporalTime = Value::undefined();
  if (item.isObject()) {
    Value timeZoneLike = TRY(Get(item.asObject(), PropertyKey(u"timeZone")));
    if (timeZoneLike.isUndefined()) {
      timeZone = TRY(ToTemporalTimeZoneIdentifier(item));
    } else {
      timeZone = TRY(ToTemporalTimeZoneIdentifier(timeZoneLike));
      temporalTime = TRY(Get(item.asObject(), PropertyKey(u"plainTime")));
    }
  } else {
    timeZone = TRY(ToTemporalTimeZoneIdentifier(item));
  }
  EpochNanoseconds epochNs;
  if (temporalTime.isUndefined()) {
    epochNs = TRY(GetStartOfDayForZone(timeZone, d->isoDate));
  } else {
    PlainTimeObject* plainTime = TRY(ToTemporalTime(temporalTime));
    ISODateTime isoDateTime = CombineISODateAndTimeRecord(d->isoDate, plainTime->time);
    if (!ISODateTimeWithinLimits(isoDateTime)) return ThrowRangeError("date-time is outside the supported range");
    epochNs = TRY(GetEpochNanosecondsForZone(timeZone, isoDateTime, Disambiguation::Compatible));
  }
  return Value(TRY(CreateTemporalZonedDateTime(epochNs, timeZone, d->calendar)));
}

Result<Value> PlainDatePrototype_toString(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.toString"));
  Object* resolvedOptions = TRY(GetOptionsObject(call.args[0]));
  ShowCalendar showCalendar = TRY(GetTemporalShowCalendarNameOption(resolvedOptions));
  return Value(newString(TemporalDateToString(d->isoDate, d->calendar, showCalendar)));
}

Result<Value> PlainDatePrototype_toLocaleString(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.toLocaleString"));
  return intl::temporalToLocaleString(Value(d), call.args[0], call.args[1]);
}

Result<Value> PlainDatePrototype_toJSON(NativeCall& call) {
  PlainDateObject* d = TRY(thisDate(call, "Temporal.PlainDate.prototype.toJSON"));
  return Value(newString(TemporalDateToString(d->isoDate, d->calendar, ShowCalendar::Auto)));
}

Result<Value> PlainDatePrototype_valueOf(NativeCall&) {
  return ThrowTypeError("Temporal.PlainDate.prototype.valueOf: use compare() or equals() to compare dates");
}

void installPlainDate(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor = defineConstructor(realm, Intrinsic::TemporalPlainDate, PlainDate_construct, 3,
                                            PropertyKey(u"PlainDate"), proto, Intrinsic::TemporalPlainDatePrototype, false);
  defineValue(TemporalNamespace(realm), PropertyKey(u"PlainDate"), Value(ctor));
  defineFunction(realm, ctor, PropertyKey(u"compare"), 2, PlainDate_compare);
  defineFunction(realm, ctor, PropertyKey(u"from"), 1, PlainDate_from);

  struct Getter {
    const char16_t* name;
    NativeFn fn;
  };
  const Getter getters[] = {
      {u"calendarId", PlainDatePrototype_calendarId},     {u"era", PlainDatePrototype_era},
      {u"eraYear", PlainDatePrototype_eraYear},           {u"year", PlainDatePrototype_year},
      {u"month", PlainDatePrototype_month},               {u"monthCode", PlainDatePrototype_monthCode},
      {u"day", PlainDatePrototype_day},                   {u"dayOfWeek", PlainDatePrototype_dayOfWeek},
      {u"dayOfYear", PlainDatePrototype_dayOfYear},       {u"weekOfYear", PlainDatePrototype_weekOfYear},
      {u"yearOfWeek", PlainDatePrototype_yearOfWeek},     {u"daysInWeek", PlainDatePrototype_daysInWeek},
      {u"daysInMonth", PlainDatePrototype_daysInMonth},   {u"daysInYear", PlainDatePrototype_daysInYear},
      {u"monthsInYear", PlainDatePrototype_monthsInYear}, {u"inLeapYear", PlainDatePrototype_inLeapYear},
  };
  for (const Getter& g : getters) defineAccessor(realm, proto, PropertyKey(g.name), g.fn, nullptr);
  defineFunction(realm, proto, PropertyKey(u"toPlainYearMonth"), 0, PlainDatePrototype_toPlainYearMonth);
  defineFunction(realm, proto, PropertyKey(u"toPlainMonthDay"), 0, PlainDatePrototype_toPlainMonthDay);
  defineFunction(realm, proto, PropertyKey(u"add"), 1, PlainDatePrototype_add);
  defineFunction(realm, proto, PropertyKey(u"subtract"), 1, PlainDatePrototype_subtract);
  defineFunction(realm, proto, PropertyKey(u"with"), 1, PlainDatePrototype_with);
  defineFunction(realm, proto, PropertyKey(u"withCalendar"), 1, PlainDatePrototype_withCalendar);
  defineFunction(realm, proto, PropertyKey(u"until"), 1, PlainDatePrototype_until);
  defineFunction(realm, proto, PropertyKey(u"since"), 1, PlainDatePrototype_since);
  defineFunction(realm, proto, PropertyKey(u"equals"), 1, PlainDatePrototype_equals);
  defineFunction(realm, proto, PropertyKey(u"toPlainDateTime"), 0, PlainDatePrototype_toPlainDateTime);
  defineFunction(realm, proto, PropertyKey(u"toZonedDateTime"), 1, PlainDatePrototype_toZonedDateTime);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, PlainDatePrototype_toString);
  defineFunction(realm, proto, PropertyKey(u"toLocaleString"), 0, PlainDatePrototype_toLocaleString);
  defineFunction(realm, proto, PropertyKey(u"toJSON"), 0, PlainDatePrototype_toJSON);
  defineFunction(realm, proto, PropertyKey(u"valueOf"), 0, PlainDatePrototype_valueOf);
  defineValue(proto, PropertyKey(builtins::symbols().toStringTag), builtins::str(u"Temporal.PlainDate"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(TemporalPlainDate, kOrderDefault, installPlainDate);

}  // namespace snowman::temporal
