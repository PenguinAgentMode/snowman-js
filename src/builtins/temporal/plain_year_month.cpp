// Temporal.PlainYearMonth (proposal-temporal §9).
#include <cmath>

#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

using builtins::num;

Result<PlainYearMonthObject*> CreateTemporalYearMonth(const ISODate& isoDate, const CalendarId& calendar, Object* newTarget) {
  if (!ISOYearMonthWithinLimits(isoDate.year, isoDate.month)) return ThrowRangeError("year-month is outside the supported range");
  if (!newTarget) newTarget = builtins::intrinsic(Intrinsic::TemporalPlainYearMonth);
  return OrdinaryCreateFromConstructor<PlainYearMonthObject>(newTarget, Intrinsic::TemporalPlainYearMonthPrototype, isoDate, calendar);
}

Result<PlainYearMonthObject*> ToTemporalYearMonth(Value item, Value options) {
  if (item.isObject()) {
    Object* o = item.asObject();
    if (auto* ym = o->as<PlainYearMonthObject>()) {
      TRY(GetTemporalOverflowOptionFrom(options));
      return CreateTemporalYearMonth(ym->isoDate, ym->calendar);
    }
    CalendarId calendar = TRY(GetTemporalCalendarIdentifierWithISODefault(o));
    CalendarFields fields = TRY(PrepareCalendarFields(calendar, o, kFieldYear | kFieldMonth | kFieldMonthCode, 0, 0u));
    Overflow overflow = TRY(GetTemporalOverflowOptionFrom(options));
    ISODate isoDate = TRY(CalendarYearMonthFromFields(calendar, fields, overflow));
    return CreateTemporalYearMonth(isoDate, calendar);
  }
  if (!item.isString()) return ThrowTypeError("a PlainYearMonth must be a Temporal object, a property bag or a string");
  ParsedISODateTime result = TRY(ParseISODateTime(item.asString()->view(), kYearMonth));
  CalendarId calendar = TRY(CanonicalizeCalendar(result.calendar ? *result.calendar : std::u16string(u"iso8601")));
  TRY(GetTemporalOverflowOptionFrom(options));
  ISODate isoDate = CreateISODateRecord(*result.year, result.month, result.day);
  if (!ISOYearMonthWithinLimits(isoDate.year, isoDate.month)) return ThrowRangeError("year-month is outside the supported range");
  CalendarFields fields = ISODateToFields(calendar, isoDate, FieldsType::YearMonth);
  isoDate = TRY(CalendarYearMonthFromFields(calendar, fields, Overflow::Constrain));
  return CreateTemporalYearMonth(isoDate, calendar);
}

namespace {

Result<Value> PlainYearMonth_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Temporal.PlainYearMonth requires 'new'");
  Value referenceISODay = call.args[3].isUndefined() ? num(1) : call.args[3];
  double y = TRY(ToIntegerWithTruncation(call.args[0]));
  double m = TRY(ToIntegerWithTruncation(call.args[1]));
  Value calendarValue = call.args[2];
  if (calendarValue.isUndefined()) calendarValue = builtins::str(u"iso8601");
  if (!calendarValue.isString()) return ThrowTypeError("calendar must be a string");
  CalendarId calendar = TRY(CanonicalizeCalendar(calendarValue.asString()->view()));
  double ref = TRY(ToIntegerWithTruncation(referenceISODay));
  if (!IsValidISODate(y, m, ref)) return ThrowRangeError("invalid date");
  if (!ISOYearMonthWithinLimits(y, int32_t(m))) return ThrowRangeError("year-month is outside the supported range");
  return Value(TRY(CreateTemporalYearMonth(CreateISODateRecord(int32_t(y), int32_t(m), int32_t(ref)), calendar, call.newTarget)));
}

Result<Value> PlainYearMonth_from(NativeCall& call) { return Value(TRY(ToTemporalYearMonth(call.args[0], call.args[1]))); }

Result<Value> PlainYearMonth_compare(NativeCall& call) {
  PlainYearMonthObject* one = TRY(ToTemporalYearMonth(call.args[0]));
  PlainYearMonthObject* two = TRY(ToTemporalYearMonth(call.args[1]));
  return num(CompareISODate(one->isoDate, two->isoDate));
}

Result<PlainYearMonthObject*> thisYearMonth(NativeCall& call, const char* method) {
  return thisTemporal<PlainYearMonthObject>(call.thisValue, method);
}

Result<Value> PlainYearMonthPrototype_calendarId(NativeCall& call) {
  PlainYearMonthObject* ym = TRY(thisYearMonth(call, "Temporal.PlainYearMonth.prototype.calendarId"));
  return Value(newString(ym->calendar));
}

#define DATE_FIELD_GETTER(name)                                                                    \
  Result<Value> PlainYearMonthPrototype_##name(NativeCall& call) {                                \
    PlainYearMonthObject* ym = TRY(thisYearMonth(call, "Temporal.PlainYearMonth.prototype." #name)); \
    return CalendarDateField(ym->calendar, ym->isoDate, u## #name);                               \
  }
DATE_FIELD_GETTER(era)
DATE_FIELD_GETTER(eraYear)
DATE_FIELD_GETTER(year)
DATE_FIELD_GETTER(month)
DATE_FIELD_GETTER(monthCode)
DATE_FIELD_GETTER(daysInYear)
DATE_FIELD_GETTER(daysInMonth)
DATE_FIELD_GETTER(monthsInYear)
DATE_FIELD_GETTER(inLeapYear)
#undef DATE_FIELD_GETTER

Result<Value> PlainYearMonthPrototype_with(NativeCall& call) {
  PlainYearMonthObject* ym = TRY(thisYearMonth(call, "Temporal.PlainYearMonth.prototype.with"));
  if (!TRY(IsPartialTemporalObject(call.args[0]))) return ThrowTypeError("with() requires a partial year-month object");
  const CalendarId& calendar = ym->calendar;
  CalendarFields fields = ISODateToFields(calendar, ym->isoDate, FieldsType::YearMonth);
  CalendarFields partialYearMonth = TRY(PrepareCalendarFields(calendar, call.args[0].asObject(),
                                                              kFieldYear | kFieldMonth | kFieldMonthCode, 0, std::nullopt));
  fields = CalendarMergeFields(calendar, fields, partialYearMonth);
  Overflow overflow = TRY(GetTemporalOverflowOptionFrom(call.args[1]));
  ISODate isoDate = TRY(CalendarYearMonthFromFields(calendar, fields, overflow));
  return Value(TRY(CreateTemporalYearMonth(isoDate, calendar)));
}

// AddDurationToYearMonth(operation, yearMonth, temporalDurationLike, options)
Result<Value> AddDurationToYearMonth(ArithmeticOperation operation, PlainYearMonthObject* yearMonth, Value temporalDurationLike,
                                     Value options) {
  DurationObject* duration = TRY(ToTemporalDuration(temporalDurationLike));
  if (operation == ArithmeticOperation::Subtract) duration = CreateNegatedTemporalDuration(duration);
  Overflow overflow = TRY(GetTemporalOverflowOptionFrom(options));
  const DurationFields& f = duration->fields;
  if (f.weeks != 0 || f.days != 0 || f.hours != 0 || f.minutes != 0 || f.seconds != 0 || f.milliseconds != 0 ||
      f.microseconds != 0 || f.nanoseconds != 0)
    return ThrowRangeError("only years and months can be added to a PlainYearMonth");
  int sign = DurationSign(f);
  const CalendarId& calendar = yearMonth->calendar;
  CalendarFields fields = ISODateToFields(calendar, yearMonth->isoDate, FieldsType::YearMonth);
  fields.day = 1;
  ISODate intermediateDate = TRY(CalendarDateFromFields(calendar, fields, Overflow::Constrain));
  ISODate date = intermediateDate;
  if (sign < 0) {
    DateDuration oneMonthDuration{0, 1, 0, 0};
    ISODate nextMonth = TRY(CalendarDateAdd(calendar, intermediateDate, oneMonthDuration, Overflow::Constrain));
    date = AddDaysToISODate(nextMonth, -1);
  }
  DateDuration durationToAdd = ToDateDurationRecordWithoutTime(f);
  ISODate addedDate = TRY(CalendarDateAdd(calendar, date, durationToAdd, overflow));
  CalendarFields addedDateFields = ISODateToFields(calendar, addedDate, FieldsType::YearMonth);
  ISODate isoDate = TRY(CalendarYearMonthFromFields(calendar, addedDateFields, overflow));
  return Value(TRY(CreateTemporalYearMonth(isoDate, calendar)));
}

Result<Value> PlainYearMonthPrototype_add(NativeCall& call) {
  PlainYearMonthObject* ym = TRY(thisYearMonth(call, "Temporal.PlainYearMonth.prototype.add"));
  return AddDurationToYearMonth(ArithmeticOperation::Add, ym, call.args[0], call.args[1]);
}

Result<Value> PlainYearMonthPrototype_subtract(NativeCall& call) {
  PlainYearMonthObject* ym = TRY(thisYearMonth(call, "Temporal.PlainYearMonth.prototype.subtract"));
  return AddDurationToYearMonth(ArithmeticOperation::Subtract, ym, call.args[0], call.args[1]);
}

// DifferenceTemporalPlainYearMonth(operation, yearMonth, other, options)
Result<Value> DifferenceTemporalPlainYearMonth(DifferenceOperation operation, PlainYearMonthObject* yearMonth,
                                               Value otherValue, Value options) {
  PlainYearMonthObject* other = TRY(ToTemporalYearMonth(otherValue));
  const CalendarId& calendar = yearMonth->calendar;
  if (!CalendarEquals(calendar, other->calendar))
    return ThrowRangeError("cannot compute the difference between year-months of different calendars");
  Object* resolvedOptions = TRY(GetOptionsObject(options));
  DifferenceSettings settings = TRY(GetDifferenceSettings(operation, resolvedOptions, UnitGroup::Date,
                                                          unitBit(Unit::Week) | unitBit(Unit::Day), Unit::Month, Unit::Year));
  if (CompareISODate(yearMonth->isoDate, other->isoDate) == 0) return Value(TRY(CreateTemporalDuration(DurationFields{})));
  CalendarFields thisFields = ISODateToFields(calendar, yearMonth->isoDate, FieldsType::YearMonth);
  thisFields.day = 1;
  ISODate thisDate = TRY(CalendarDateFromFields(calendar, thisFields, Overflow::Constrain));
  CalendarFields otherFields = ISODateToFields(calendar, other->isoDate, FieldsType::YearMonth);
  otherFields.day = 1;
  ISODate otherDate = TRY(CalendarDateFromFields(calendar, otherFields, Overflow::Constrain));
  DateDuration dateDifference = CalendarDateUntil(calendar, thisDate, otherDate, settings.largestUnit);
  DateDuration yearsMonthsDifference = TRY(AdjustDateDurationRecord(dateDifference, 0, 0));
  InternalDuration duration = CombineDateAndTimeDuration(yearsMonthsDifference, 0);
  if (settings.smallestUnit != Unit::Month || settings.roundingIncrement != 1) {
    ISODateTime isoDateTime = CombineISODateAndTimeRecord(thisDate, MidnightTimeRecord());
    ISODateTime isoDateTimeOther = CombineISODateAndTimeRecord(otherDate, MidnightTimeRecord());
    EpochNanoseconds originEpochNs = GetUTCEpochNanoseconds(isoDateTime);
    EpochNanoseconds destEpochNs = GetUTCEpochNanoseconds(isoDateTimeOther);
    duration = TRY(RoundRelativeDuration(duration, originEpochNs, destEpochNs, isoDateTime, nullptr, calendar,
                                         settings.largestUnit, settings.roundingIncrement, settings.smallestUnit,
                                         settings.roundingMode));
  }
  DurationObject* result = TRY(TemporalDurationFromInternal(duration, Unit::Day));
  if (operation == DifferenceOperation::Since) result = CreateNegatedTemporalDuration(result);
  return Value(result);
}

Result<Value> PlainYearMonthPrototype_until(NativeCall& call) {
  PlainYearMonthObject* ym = TRY(thisYearMonth(call, "Temporal.PlainYearMonth.prototype.until"));
  return DifferenceTemporalPlainYearMonth(DifferenceOperation::Until, ym, call.args[0], call.args[1]);
}

Result<Value> PlainYearMonthPrototype_since(NativeCall& call) {
  PlainYearMonthObject* ym = TRY(thisYearMonth(call, "Temporal.PlainYearMonth.prototype.since"));
  return DifferenceTemporalPlainYearMonth(DifferenceOperation::Since, ym, call.args[0], call.args[1]);
}

Result<Value> PlainYearMonthPrototype_equals(NativeCall& call) {
  PlainYearMonthObject* ym = TRY(thisYearMonth(call, "Temporal.PlainYearMonth.prototype.equals"));
  PlainYearMonthObject* other = TRY(ToTemporalYearMonth(call.args[0]));
  if (CompareISODate(ym->isoDate, other->isoDate) != 0) return Value::boolean(false);
  return Value::boolean(CalendarEquals(ym->calendar, other->calendar));
}

Result<Value> PlainYearMonthPrototype_toString(NativeCall& call) {
  PlainYearMonthObject* ym = TRY(thisYearMonth(call, "Temporal.PlainYearMonth.prototype.toString"));
  Object* resolvedOptions = TRY(GetOptionsObject(call.args[0]));
  ShowCalendar showCalendar = TRY(GetTemporalShowCalendarNameOption(resolvedOptions));
  return Value(newString(TemporalYearMonthToString(ym->isoDate, ym->calendar, showCalendar)));
}

Result<Value> PlainYearMonthPrototype_toLocaleString(NativeCall& call) {
  PlainYearMonthObject* ym = TRY(thisYearMonth(call, "Temporal.PlainYearMonth.prototype.toLocaleString"));
  return Value(newString(TemporalYearMonthToString(ym->isoDate, ym->calendar, ShowCalendar::Auto)));
}

Result<Value> PlainYearMonthPrototype_toJSON(NativeCall& call) {
  PlainYearMonthObject* ym = TRY(thisYearMonth(call, "Temporal.PlainYearMonth.prototype.toJSON"));
  return Value(newString(TemporalYearMonthToString(ym->isoDate, ym->calendar, ShowCalendar::Auto)));
}

Result<Value> PlainYearMonthPrototype_valueOf(NativeCall&) {
  return ThrowTypeError("Temporal.PlainYearMonth.prototype.valueOf: use compare() or equals() to compare year-months");
}

Result<Value> PlainYearMonthPrototype_toPlainDate(NativeCall& call) {
  PlainYearMonthObject* ym = TRY(thisYearMonth(call, "Temporal.PlainYearMonth.prototype.toPlainDate"));
  if (!call.args[0].isObject()) return ThrowTypeError("toPlainDate requires an object with a day property");
  const CalendarId& calendar = ym->calendar;
  CalendarFields fields = ISODateToFields(calendar, ym->isoDate, FieldsType::YearMonth);
  CalendarFields inputFields = TRY(PrepareCalendarFields(calendar, call.args[0].asObject(), kFieldDay, 0, 0u));
  CalendarFields mergedFields = CalendarMergeFields(calendar, fields, inputFields);
  ISODate isoDate = TRY(CalendarDateFromFields(calendar, mergedFields, Overflow::Constrain));
  return Value(TRY(CreateTemporalDate(isoDate, calendar)));
}

void installPlainYearMonth(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor =
      defineConstructor(realm, Intrinsic::TemporalPlainYearMonth, PlainYearMonth_construct, 2, PropertyKey(u"PlainYearMonth"),
                        proto, Intrinsic::TemporalPlainYearMonthPrototype, false);
  defineValue(TemporalNamespace(realm), PropertyKey(u"PlainYearMonth"), Value(ctor));
  defineFunction(realm, ctor, PropertyKey(u"compare"), 2, PlainYearMonth_compare);
  defineFunction(realm, ctor, PropertyKey(u"from"), 1, PlainYearMonth_from);
  struct Getter {
    const char16_t* name;
    NativeFn fn;
  };
  const Getter getters[] = {
      {u"calendarId", PlainYearMonthPrototype_calendarId},   {u"era", PlainYearMonthPrototype_era},
      {u"eraYear", PlainYearMonthPrototype_eraYear},         {u"year", PlainYearMonthPrototype_year},
      {u"month", PlainYearMonthPrototype_month},             {u"monthCode", PlainYearMonthPrototype_monthCode},
      {u"daysInYear", PlainYearMonthPrototype_daysInYear},   {u"daysInMonth", PlainYearMonthPrototype_daysInMonth},
      {u"monthsInYear", PlainYearMonthPrototype_monthsInYear}, {u"inLeapYear", PlainYearMonthPrototype_inLeapYear},
  };
  for (const Getter& g : getters) defineAccessor(realm, proto, PropertyKey(g.name), g.fn, nullptr);
  defineFunction(realm, proto, PropertyKey(u"with"), 1, PlainYearMonthPrototype_with);
  defineFunction(realm, proto, PropertyKey(u"add"), 1, PlainYearMonthPrototype_add);
  defineFunction(realm, proto, PropertyKey(u"subtract"), 1, PlainYearMonthPrototype_subtract);
  defineFunction(realm, proto, PropertyKey(u"until"), 1, PlainYearMonthPrototype_until);
  defineFunction(realm, proto, PropertyKey(u"since"), 1, PlainYearMonthPrototype_since);
  defineFunction(realm, proto, PropertyKey(u"equals"), 1, PlainYearMonthPrototype_equals);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, PlainYearMonthPrototype_toString);
  defineFunction(realm, proto, PropertyKey(u"toLocaleString"), 0, PlainYearMonthPrototype_toLocaleString);
  defineFunction(realm, proto, PropertyKey(u"toJSON"), 0, PlainYearMonthPrototype_toJSON);
  defineFunction(realm, proto, PropertyKey(u"valueOf"), 0, PlainYearMonthPrototype_valueOf);
  defineFunction(realm, proto, PropertyKey(u"toPlainDate"), 1, PlainYearMonthPrototype_toPlainDate);
  defineValue(proto, PropertyKey(builtins::symbols().toStringTag), builtins::str(u"Temporal.PlainYearMonth"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(TemporalPlainYearMonth, kOrderDefault, installPlainYearMonth);

}  // namespace snowman::temporal
