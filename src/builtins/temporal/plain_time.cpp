// Temporal.PlainTime (proposal-temporal §4).
#include <cmath>

#include "builtins/intl/hooks.h"
#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

using builtins::num;

Result<PlainTimeObject*> CreateTemporalTime(const TimeRecord& time, Object* newTarget) {
  if (!newTarget) newTarget = builtins::intrinsic(Intrinsic::TemporalPlainTime);
  TimeRecord t = time;
  t.days = 0;
  return OrdinaryCreateFromConstructor<PlainTimeObject>(newTarget, Intrinsic::TemporalPlainTimePrototype, t);
}

Result<PartialTime> ToTemporalTimeRecord(Object* temporalTimeLike, bool partial) {
  PartialTime result;
  bool any = false;
  struct Row {
    const char16_t* name;
    std::optional<double> PartialTime::* field;
  };
  static const Row rows[] = {
      {u"hour", &PartialTime::hour},
      {u"microsecond", &PartialTime::microsecond},
      {u"millisecond", &PartialTime::millisecond},
      {u"minute", &PartialTime::minute},
      {u"nanosecond", &PartialTime::nanosecond},
      {u"second", &PartialTime::second},
  };
  for (const Row& row : rows) {
    Value v = TRY(Get(temporalTimeLike, PropertyKey(row.name)));
    if (!v.isUndefined()) {
      result.*row.field = TRY(ToIntegerWithTruncation(v));
      any = true;
    }
  }
  if (!any) return ThrowTypeError("time-like object must have at least one time property");
  if (!partial) {
    for (const Row& row : rows)
      if (!(result.*row.field)) result.*row.field = 0;
  }
  return result;
}

Result<PlainTimeObject*> ToTemporalTime(Value item, Value options) {
  TimeRecord result;
  if (item.isObject()) {
    Object* o = item.asObject();
    if (auto* t = o->as<PlainTimeObject>()) {
      TRY(GetTemporalOverflowOptionFrom(options));
      return CreateTemporalTime(t->time);
    }
    if (auto* dt = o->as<PlainDateTimeObject>()) {
      TRY(GetTemporalOverflowOptionFrom(options));
      return CreateTemporalTime(dt->isoDateTime.time);
    }
    if (auto* z = o->as<ZonedDateTimeObject>()) {
      ISODateTime isoDateTime = TRY(GetISODateTimeForZone(z->timeZone, z->epochNanoseconds));
      TRY(GetTemporalOverflowOptionFrom(options));
      return CreateTemporalTime(isoDateTime.time);
    }
    PartialTime r = TRY(ToTemporalTimeRecord(o, false));
    Overflow overflow = TRY(GetTemporalOverflowOptionFrom(options));
    result = TRY(RegulateTime(*r.hour, *r.minute, *r.second, *r.millisecond, *r.microsecond, *r.nanosecond, overflow));
  } else {
    if (!item.isString()) return ThrowTypeError("a PlainTime must be a Temporal object, a property bag or a string");
    ParsedISODateTime parsed = TRY(ParseISODateTime(item.asString()->view(), kTime));
    TRY(GetTemporalOverflowOptionFrom(options));
    result = parsed.time ? *parsed.time : MidnightTimeRecord();
  }
  return CreateTemporalTime(result);
}

namespace {

Result<Value> PlainTime_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Temporal.PlainTime requires 'new'");
  double v[6] = {};
  for (size_t i = 0; i < 6; i++)
    if (!call.args[i].isUndefined()) v[i] = TRY(ToIntegerWithTruncation(call.args[i]));
  if (!IsValidTime(v[0], v[1], v[2], v[3], v[4], v[5])) return ThrowRangeError("invalid time");
  TimeRecord time = CreateTimeRecord(int32_t(v[0]), int32_t(v[1]), int32_t(v[2]), int32_t(v[3]), int32_t(v[4]), int32_t(v[5]));
  return Value(TRY(CreateTemporalTime(time, call.newTarget)));
}

Result<Value> PlainTime_from(NativeCall& call) { return Value(TRY(ToTemporalTime(call.args[0], call.args[1]))); }

Result<Value> PlainTime_compare(NativeCall& call) {
  PlainTimeObject* one = TRY(ToTemporalTime(call.args[0]));
  PlainTimeObject* two = TRY(ToTemporalTime(call.args[1]));
  return num(CompareTimeRecord(one->time, two->time));
}

Result<PlainTimeObject*> thisTime(NativeCall& call, const char* method) {
  return thisTemporal<PlainTimeObject>(call.thisValue, method);
}

#define TIME_GETTER(name)                                                             \
  Result<Value> PlainTimePrototype_##name(NativeCall& call) {                        \
    PlainTimeObject* t = TRY(thisTime(call, "Temporal.PlainTime.prototype." #name)); \
    return num(t->time.name);                                                        \
  }
TIME_GETTER(hour)
TIME_GETTER(minute)
TIME_GETTER(second)
TIME_GETTER(millisecond)
TIME_GETTER(microsecond)
TIME_GETTER(nanosecond)
#undef TIME_GETTER

// AddDurationToTime(operation, temporalTime, temporalDurationLike)
Result<Value> AddDurationToTime(ArithmeticOperation operation, PlainTimeObject* temporalTime, Value temporalDurationLike) {
  DurationObject* duration = TRY(ToTemporalDuration(temporalDurationLike));
  if (operation == ArithmeticOperation::Subtract) duration = CreateNegatedTemporalDuration(duration);
  InternalDuration internalDuration = ToInternalDurationRecord(duration->fields);
  TimeRecord result = AddTime(temporalTime->time, internalDuration.time);
  return Value(TRY(CreateTemporalTime(result)));
}

Result<Value> PlainTimePrototype_add(NativeCall& call) {
  PlainTimeObject* t = TRY(thisTime(call, "Temporal.PlainTime.prototype.add"));
  return AddDurationToTime(ArithmeticOperation::Add, t, call.args[0]);
}

Result<Value> PlainTimePrototype_subtract(NativeCall& call) {
  PlainTimeObject* t = TRY(thisTime(call, "Temporal.PlainTime.prototype.subtract"));
  return AddDurationToTime(ArithmeticOperation::Subtract, t, call.args[0]);
}

Result<Value> PlainTimePrototype_with(NativeCall& call) {
  PlainTimeObject* t = TRY(thisTime(call, "Temporal.PlainTime.prototype.with"));
  if (!TRY(IsPartialTemporalObject(call.args[0]))) return ThrowTypeError("with() requires a partial time object");
  PartialTime p = TRY(ToTemporalTimeRecord(call.args[0].asObject(), true));
  const TimeRecord& time = t->time;
  double hour = p.hour.value_or(time.hour), minute = p.minute.value_or(time.minute), second = p.second.value_or(time.second);
  double millisecond = p.millisecond.value_or(time.millisecond), microsecond = p.microsecond.value_or(time.microsecond);
  double nanosecond = p.nanosecond.value_or(time.nanosecond);
  Overflow overflow = TRY(GetTemporalOverflowOptionFrom(call.args[1]));
  TimeRecord result = TRY(RegulateTime(hour, minute, second, millisecond, microsecond, nanosecond, overflow));
  return Value(TRY(CreateTemporalTime(result)));
}

// DifferenceTemporalPlainTime(operation, temporalTime, other, options)
Result<Value> DifferenceTemporalPlainTime(DifferenceOperation operation, PlainTimeObject* temporalTime, Value otherValue,
                                          Value options) {
  PlainTimeObject* other = TRY(ToTemporalTime(otherValue));
  Object* resolvedOptions = TRY(GetOptionsObject(options));
  DifferenceSettings settings =
      TRY(GetDifferenceSettings(operation, resolvedOptions, UnitGroup::Time, 0, Unit::Nanosecond, Unit::Hour));
  TimeDuration timeDuration = DifferenceTime(temporalTime->time, other->time);
  timeDuration = TRY(RoundTimeDuration(timeDuration, settings.roundingIncrement, settings.smallestUnit, settings.roundingMode));
  InternalDuration duration = CombineDateAndTimeDuration(DateDuration{}, timeDuration);
  DurationObject* result = TRY(TemporalDurationFromInternal(duration, settings.largestUnit));
  if (operation == DifferenceOperation::Since) result = CreateNegatedTemporalDuration(result);
  return Value(result);
}

Result<Value> PlainTimePrototype_until(NativeCall& call) {
  PlainTimeObject* t = TRY(thisTime(call, "Temporal.PlainTime.prototype.until"));
  return DifferenceTemporalPlainTime(DifferenceOperation::Until, t, call.args[0], call.args[1]);
}

Result<Value> PlainTimePrototype_since(NativeCall& call) {
  PlainTimeObject* t = TRY(thisTime(call, "Temporal.PlainTime.prototype.since"));
  return DifferenceTemporalPlainTime(DifferenceOperation::Since, t, call.args[0], call.args[1]);
}

Result<Value> PlainTimePrototype_round(NativeCall& call) {
  PlainTimeObject* t = TRY(thisTime(call, "Temporal.PlainTime.prototype.round"));
  Object* roundTo = TRY(GetRoundingOptionsArgument(call.args[0], u"smallestUnit", "Temporal.PlainTime.prototype.round"));
  int64_t roundingIncrement = TRY(GetRoundingIncrementOption(roundTo));
  RoundingMode roundingMode = TRY(GetRoundingModeOption(roundTo, RoundingMode::HalfExpand));
  Unit smallestUnit = TRY(GetTemporalUnitValuedOption(roundTo, u"smallestUnit", true));
  TRY(ValidateTemporalUnitValue(smallestUnit, UnitGroup::Time));
  int64_t maximum = MaximumTemporalDurationRoundingIncrement(smallestUnit);
  TRY(ValidateTemporalRoundingIncrement(roundingIncrement, maximum, false));
  TimeRecord result = RoundTime(t->time, roundingIncrement, smallestUnit, roundingMode);
  return Value(TRY(CreateTemporalTime(result)));
}

Result<Value> PlainTimePrototype_equals(NativeCall& call) {
  PlainTimeObject* t = TRY(thisTime(call, "Temporal.PlainTime.prototype.equals"));
  PlainTimeObject* other = TRY(ToTemporalTime(call.args[0]));
  return Value::boolean(CompareTimeRecord(t->time, other->time) == 0);
}

Result<Value> PlainTimePrototype_toString(NativeCall& call) {
  PlainTimeObject* t = TRY(thisTime(call, "Temporal.PlainTime.prototype.toString"));
  Object* resolvedOptions = TRY(GetOptionsObject(call.args[0]));
  int digits = TRY(GetTemporalFractionalSecondDigitsOption(resolvedOptions));
  RoundingMode roundingMode = TRY(GetRoundingModeOption(resolvedOptions, RoundingMode::Trunc));
  Unit smallestUnit = TRY(GetTemporalUnitValuedOption(resolvedOptions, u"smallestUnit"));
  TRY(ValidateTemporalUnitValue(smallestUnit, UnitGroup::Time));
  if (smallestUnit == Unit::Hour) return ThrowRangeError("smallestUnit must be minutes or smaller");
  SecondsStringPrecision precision = TRY(ToSecondsStringPrecisionRecord(smallestUnit, digits));
  TimeRecord roundResult = RoundTime(t->time, precision.increment, precision.unit, roundingMode);
  return Value(newString(TimeRecordToString(roundResult, precision.precision)));
}

Result<Value> PlainTimePrototype_toLocaleString(NativeCall& call) {
  PlainTimeObject* t = TRY(thisTime(call, "Temporal.PlainTime.prototype.toLocaleString"));
  return intl::temporalToLocaleString(Value(t), call.args[0], call.args[1]);
}

Result<Value> PlainTimePrototype_toJSON(NativeCall& call) {
  PlainTimeObject* t = TRY(thisTime(call, "Temporal.PlainTime.prototype.toJSON"));
  return Value(newString(TimeRecordToString(t->time, -1)));
}

Result<Value> PlainTimePrototype_valueOf(NativeCall&) {
  return ThrowTypeError("Temporal.PlainTime.prototype.valueOf: use compare() or equals() to compare times");
}

void installPlainTime(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor = defineConstructor(realm, Intrinsic::TemporalPlainTime, PlainTime_construct, 0,
                                            PropertyKey(u"PlainTime"), proto, Intrinsic::TemporalPlainTimePrototype, false);
  defineValue(TemporalNamespace(realm), PropertyKey(u"PlainTime"), Value(ctor));
  defineFunction(realm, ctor, PropertyKey(u"compare"), 2, PlainTime_compare);
  defineFunction(realm, ctor, PropertyKey(u"from"), 1, PlainTime_from);
  defineAccessor(realm, proto, PropertyKey(u"hour"), PlainTimePrototype_hour, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"minute"), PlainTimePrototype_minute, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"second"), PlainTimePrototype_second, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"millisecond"), PlainTimePrototype_millisecond, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"microsecond"), PlainTimePrototype_microsecond, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"nanosecond"), PlainTimePrototype_nanosecond, nullptr);
  defineFunction(realm, proto, PropertyKey(u"add"), 1, PlainTimePrototype_add);
  defineFunction(realm, proto, PropertyKey(u"subtract"), 1, PlainTimePrototype_subtract);
  defineFunction(realm, proto, PropertyKey(u"with"), 1, PlainTimePrototype_with);
  defineFunction(realm, proto, PropertyKey(u"until"), 1, PlainTimePrototype_until);
  defineFunction(realm, proto, PropertyKey(u"since"), 1, PlainTimePrototype_since);
  defineFunction(realm, proto, PropertyKey(u"round"), 1, PlainTimePrototype_round);
  defineFunction(realm, proto, PropertyKey(u"equals"), 1, PlainTimePrototype_equals);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, PlainTimePrototype_toString);
  defineFunction(realm, proto, PropertyKey(u"toLocaleString"), 0, PlainTimePrototype_toLocaleString);
  defineFunction(realm, proto, PropertyKey(u"toJSON"), 0, PlainTimePrototype_toJSON);
  defineFunction(realm, proto, PropertyKey(u"valueOf"), 0, PlainTimePrototype_valueOf);
  defineValue(proto, PropertyKey(builtins::symbols().toStringTag), builtins::str(u"Temporal.PlainTime"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(TemporalPlainTime, kOrderDefault, installPlainTime);

}  // namespace snowman::temporal
