// Temporal.PlainMonthDay (proposal-temporal §10).
#include <cmath>

#include "builtins/intl/hooks.h"
#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

using builtins::num;

Result<PlainMonthDayObject*> CreateTemporalMonthDay(const ISODate& isoDate, const CalendarId& calendar, Object* newTarget) {
  if (!ISODateWithinLimits(isoDate)) return ThrowRangeError("month-day is outside the supported range");
  if (!newTarget) newTarget = builtins::intrinsic(Intrinsic::TemporalPlainMonthDay);
  return OrdinaryCreateFromConstructor<PlainMonthDayObject>(newTarget, Intrinsic::TemporalPlainMonthDayPrototype, isoDate, calendar);
}

Result<PlainMonthDayObject*> ToTemporalMonthDay(Value item, Value options) {
  if (item.isObject()) {
    Object* o = item.asObject();
    if (auto* md = o->as<PlainMonthDayObject>()) {
      TRY(GetTemporalOverflowOptionFrom(options));
      return CreateTemporalMonthDay(md->isoDate, md->calendar);
    }
    CalendarId calendar = TRY(GetTemporalCalendarIdentifierWithISODefault(o));
    CalendarFields fields = TRY(PrepareCalendarFields(calendar, o, kFieldsDate, 0, 0u));
    Overflow overflow = TRY(GetTemporalOverflowOptionFrom(options));
    ISODate isoDate = TRY(CalendarMonthDayFromFields(calendar, fields, overflow));
    return CreateTemporalMonthDay(isoDate, calendar);
  }
  if (!item.isString()) return ThrowTypeError("a PlainMonthDay must be a Temporal object, a property bag or a string");
  ParsedISODateTime result = TRY(ParseISODateTime(item.asString()->view(), kMonthDay));
  CalendarId calendar = TRY(CanonicalizeCalendar(result.calendar ? *result.calendar : std::u16string(u"iso8601")));
  TRY(GetTemporalOverflowOptionFrom(options));
  if (calendar == u"iso8601") return CreateTemporalMonthDay(CreateISODateRecord(1972, result.month, result.day), calendar);
  ISODate isoDate = CreateISODateRecord(result.year.value_or(1972), result.month, result.day);
  if (!ISODateWithinLimits(isoDate)) return ThrowRangeError("month-day is outside the supported range");
  CalendarFields fields = ISODateToFields(calendar, isoDate, FieldsType::MonthDay);
  isoDate = TRY(CalendarMonthDayFromFields(calendar, fields, Overflow::Constrain));
  return CreateTemporalMonthDay(isoDate, calendar);
}

namespace {

Result<Value> PlainMonthDay_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Temporal.PlainMonthDay requires 'new'");
  Value referenceISOYear = call.args[3].isUndefined() ? num(1972) : call.args[3];
  double m = TRY(ToIntegerWithTruncation(call.args[0]));
  double d = TRY(ToIntegerWithTruncation(call.args[1]));
  Value calendarValue = call.args[2];
  if (calendarValue.isUndefined()) calendarValue = builtins::str(u"iso8601");
  if (!calendarValue.isString()) return ThrowTypeError("calendar must be a string");
  CalendarId calendar = TRY(CanonicalizeCalendar(calendarValue.asString()->view()));
  double y = TRY(ToIntegerWithTruncation(referenceISOYear));
  if (!IsValidISODate(y, m, d)) return ThrowRangeError("invalid date");
  if (!ISODateWithinLimits(y, int32_t(m), int32_t(d))) return ThrowRangeError("month-day is outside the supported range");
  return Value(TRY(CreateTemporalMonthDay(CreateISODateRecord(int32_t(y), int32_t(m), int32_t(d)), calendar, call.newTarget)));
}

Result<Value> PlainMonthDay_from(NativeCall& call) { return Value(TRY(ToTemporalMonthDay(call.args[0], call.args[1]))); }

Result<PlainMonthDayObject*> thisMonthDay(NativeCall& call, const char* method) {
  return thisTemporal<PlainMonthDayObject>(call.thisValue, method);
}

Result<Value> PlainMonthDayPrototype_calendarId(NativeCall& call) {
  PlainMonthDayObject* md = TRY(thisMonthDay(call, "Temporal.PlainMonthDay.prototype.calendarId"));
  return Value(newString(md->calendar));
}

Result<Value> PlainMonthDayPrototype_monthCode(NativeCall& call) {
  PlainMonthDayObject* md = TRY(thisMonthDay(call, "Temporal.PlainMonthDay.prototype.monthCode"));
  return CalendarDateField(md->calendar, md->isoDate, u"monthCode");
}

Result<Value> PlainMonthDayPrototype_day(NativeCall& call) {
  PlainMonthDayObject* md = TRY(thisMonthDay(call, "Temporal.PlainMonthDay.prototype.day"));
  return CalendarDateField(md->calendar, md->isoDate, u"day");
}

Result<Value> PlainMonthDayPrototype_with(NativeCall& call) {
  PlainMonthDayObject* md = TRY(thisMonthDay(call, "Temporal.PlainMonthDay.prototype.with"));
  if (!TRY(IsPartialTemporalObject(call.args[0]))) return ThrowTypeError("with() requires a partial month-day object");
  const CalendarId& calendar = md->calendar;
  CalendarFields fields = ISODateToFields(calendar, md->isoDate, FieldsType::MonthDay);
  CalendarFields partialMonthDay = TRY(PrepareCalendarFields(calendar, call.args[0].asObject(), kFieldsDate, 0, std::nullopt));
  fields = CalendarMergeFields(calendar, fields, partialMonthDay);
  Overflow overflow = TRY(GetTemporalOverflowOptionFrom(call.args[1]));
  ISODate isoDate = TRY(CalendarMonthDayFromFields(calendar, fields, overflow));
  return Value(TRY(CreateTemporalMonthDay(isoDate, calendar)));
}

Result<Value> PlainMonthDayPrototype_equals(NativeCall& call) {
  PlainMonthDayObject* md = TRY(thisMonthDay(call, "Temporal.PlainMonthDay.prototype.equals"));
  PlainMonthDayObject* other = TRY(ToTemporalMonthDay(call.args[0]));
  if (CompareISODate(md->isoDate, other->isoDate) != 0) return Value::boolean(false);
  return Value::boolean(CalendarEquals(md->calendar, other->calendar));
}

Result<Value> PlainMonthDayPrototype_toString(NativeCall& call) {
  PlainMonthDayObject* md = TRY(thisMonthDay(call, "Temporal.PlainMonthDay.prototype.toString"));
  Object* resolvedOptions = TRY(GetOptionsObject(call.args[0]));
  ShowCalendar showCalendar = TRY(GetTemporalShowCalendarNameOption(resolvedOptions));
  return Value(newString(TemporalMonthDayToString(md->isoDate, md->calendar, showCalendar)));
}

Result<Value> PlainMonthDayPrototype_toLocaleString(NativeCall& call) {
  PlainMonthDayObject* md = TRY(thisMonthDay(call, "Temporal.PlainMonthDay.prototype.toLocaleString"));
  return intl::temporalToLocaleString(Value(md), call.args[0], call.args[1]);
}

Result<Value> PlainMonthDayPrototype_toJSON(NativeCall& call) {
  PlainMonthDayObject* md = TRY(thisMonthDay(call, "Temporal.PlainMonthDay.prototype.toJSON"));
  return Value(newString(TemporalMonthDayToString(md->isoDate, md->calendar, ShowCalendar::Auto)));
}

Result<Value> PlainMonthDayPrototype_valueOf(NativeCall&) {
  return ThrowTypeError("Temporal.PlainMonthDay.prototype.valueOf: use equals() to compare month-days");
}

Result<Value> PlainMonthDayPrototype_toPlainDate(NativeCall& call) {
  PlainMonthDayObject* md = TRY(thisMonthDay(call, "Temporal.PlainMonthDay.prototype.toPlainDate"));
  if (!call.args[0].isObject()) return ThrowTypeError("toPlainDate requires an object with a year property");
  const CalendarId& calendar = md->calendar;
  CalendarFields fields = ISODateToFields(calendar, md->isoDate, FieldsType::MonthDay);
  CalendarFields inputFields = TRY(PrepareCalendarFields(calendar, call.args[0].asObject(), kFieldYear, 0, 0u));
  CalendarFields mergedFields = CalendarMergeFields(calendar, fields, inputFields);
  ISODate isoDate = TRY(CalendarDateFromFields(calendar, mergedFields, Overflow::Constrain));
  return Value(TRY(CreateTemporalDate(isoDate, calendar)));
}

void installPlainMonthDay(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor =
      defineConstructor(realm, Intrinsic::TemporalPlainMonthDay, PlainMonthDay_construct, 2, PropertyKey(u"PlainMonthDay"),
                        proto, Intrinsic::TemporalPlainMonthDayPrototype, false);
  defineValue(TemporalNamespace(realm), PropertyKey(u"PlainMonthDay"), Value(ctor));
  defineFunction(realm, ctor, PropertyKey(u"from"), 1, PlainMonthDay_from);
  defineAccessor(realm, proto, PropertyKey(u"calendarId"), PlainMonthDayPrototype_calendarId, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"monthCode"), PlainMonthDayPrototype_monthCode, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"day"), PlainMonthDayPrototype_day, nullptr);
  defineFunction(realm, proto, PropertyKey(u"with"), 1, PlainMonthDayPrototype_with);
  defineFunction(realm, proto, PropertyKey(u"equals"), 1, PlainMonthDayPrototype_equals);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, PlainMonthDayPrototype_toString);
  defineFunction(realm, proto, PropertyKey(u"toLocaleString"), 0, PlainMonthDayPrototype_toLocaleString);
  defineFunction(realm, proto, PropertyKey(u"toJSON"), 0, PlainMonthDayPrototype_toJSON);
  defineFunction(realm, proto, PropertyKey(u"valueOf"), 0, PlainMonthDayPrototype_valueOf);
  defineFunction(realm, proto, PropertyKey(u"toPlainDate"), 1, PlainMonthDayPrototype_toPlainDate);
  defineValue(proto, PropertyKey(builtins::symbols().toStringTag), builtins::str(u"Temporal.PlainMonthDay"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(TemporalPlainMonthDay, kOrderDefault, installPlainMonthDay);

}  // namespace snowman::temporal
