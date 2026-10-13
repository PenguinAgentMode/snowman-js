// Temporal.Instant (proposal-temporal §8): an exact time, nanoseconds since the epoch.
#include <cmath>

#include "builtins/intl/hooks.h"
#include "builtins/temporal/temporal.h"
#include "runtime/bigint.h"

namespace snowman::temporal {

using builtins::num;
using builtins::str;

Result<InstantObject*> CreateTemporalInstant(EpochNanoseconds epochNanoseconds, Object* newTarget) {
  Object* prototype = newTarget ? TRY(GetPrototypeFromConstructor(newTarget, Intrinsic::TemporalInstantPrototype))
                                : builtins::intrinsic(Intrinsic::TemporalInstantPrototype);
  return make<InstantObject>(prototype, epochNanoseconds);
}

namespace {

// CheckISODaysRange
bool ISODaysInRange(const ISODate& date) {
  int64_t days = ISODateToEpochDays(date.year, date.month - 1, date.day);
  return days >= -100'000'000 && days <= 100'000'000;
}

// ToTemporalInstant
Result<InstantObject*> ToTemporalInstant(Value item) {
  if (item.isObject()) {
    if (auto* instant = asTemporal<InstantObject>(item)) return CreateTemporalInstant(instant->epochNanoseconds);
    if (auto* zoned = asTemporal<ZonedDateTimeObject>(item)) return CreateTemporalInstant(zoned->epochNanoseconds);
    item = TRY(ToPrimitive(item, PreferredType::String));
  }
  if (!item.isString()) return ThrowTypeError("an Instant must be a Temporal.Instant or an ISO string");
  ParsedISODateTime parsed = TRY(ParseISODateTime(item.asString()->view(), kInstant));
  int64_t offsetNanoseconds = parsed.timeZone.z ? 0 : TRY(ParseDateTimeUTCOffset(*parsed.timeZone.offsetString));
  const TimeRecord& time = *parsed.time;
  ISODateTime balanced = BalanceISODateTime(ISODate{*parsed.year, parsed.month, parsed.day}, time.hour, time.minute,
                                            time.second, time.millisecond, time.microsecond,
                                            Int128(time.nanosecond) - offsetNanoseconds);
  if (!ISODaysInRange(balanced.isoDate)) return ThrowRangeError("instant is outside the supported range");
  EpochNanoseconds epochNanoseconds = GetUTCEpochNanoseconds(balanced);
  if (!IsValidEpochNanoseconds(epochNanoseconds)) return ThrowRangeError("instant is outside the supported range");
  return CreateTemporalInstant(epochNanoseconds);
}

// The validated epoch nanoseconds of a BigInt.
Result<EpochNanoseconds> ToEpochNanoseconds(BigInt* bigint) {
  std::optional<EpochNanoseconds> ns = BigIntToEpochNanoseconds(bigint);
  if (!ns || !IsValidEpochNanoseconds(*ns)) return ThrowRangeError("epoch nanoseconds are outside the supported range");
  return *ns;
}

int CompareEpochNanoseconds(EpochNanoseconds one, EpochNanoseconds two) { return one > two ? 1 : one < two ? -1 : 0; }

// AddInstant
Result<EpochNanoseconds> AddInstant(EpochNanoseconds epochNanoseconds, TimeDuration timeDuration) {
  EpochNanoseconds result = AddTimeDurationToEpochNanoseconds(timeDuration, epochNanoseconds);
  if (!IsValidEpochNanoseconds(result)) return ThrowRangeError("instant is outside the supported range");
  return result;
}

// RoundTemporalInstant
EpochNanoseconds RoundTemporalInstant(EpochNanoseconds ns, int64_t increment, Unit unit, RoundingMode roundingMode) {
  return RoundNumberToIncrementAsIfPositive(ns, Int128(increment) * UnitLengthInNanoseconds(unit), roundingMode);
}

// TemporalInstantToString(instant, timeZone, precision); timeZone nullptr = undefined.
std::u16string TemporalInstantToString(EpochNanoseconds epochNs, const TimeZone* timeZone, int precision) {
  TimeZone outputTimeZone = timeZone ? *timeZone : TimeZone::UTC();
  ISODateTime isoDateTime = GetISODateTimeFor(outputTimeZone, epochNs);
  std::u16string result = ISODateTimeToString(isoDateTime, IsoCalendar(), precision, ShowCalendar::Never);
  if (!timeZone) return result + u"Z";
  return result + FormatDateTimeUTCOffsetRounded(GetOffsetNanosecondsFor(outputTimeZone, epochNs));
}

Result<InstantObject*> thisInstant(NativeCall& call, const char* method) {
  return thisTemporal<InstantObject>(call.thisValue, method);
}

// DifferenceTemporalInstant
Result<Value> DifferenceTemporalInstant(DifferenceOperation operation, InstantObject* instant, Value otherValue, Value options) {
  InstantObject* other = TRY(ToTemporalInstant(otherValue));
  Object* resolvedOptions = TRY(GetOptionsObject(options));
  DifferenceSettings settings =
      TRY(GetDifferenceSettings(operation, resolvedOptions, UnitGroup::Time, 0, Unit::Nanosecond, Unit::Second));
  // DifferenceInstant
  TimeDuration timeDuration = TimeDurationFromEpochNanosecondsDifference(other->epochNanoseconds, instant->epochNanoseconds);
  timeDuration = TRY(RoundTimeDuration(timeDuration, settings.roundingIncrement, settings.smallestUnit, settings.roundingMode));
  InternalDuration internalDuration = CombineDateAndTimeDuration(DateDuration{}, timeDuration);
  DurationObject* result = TRY(TemporalDurationFromInternal(internalDuration, settings.largestUnit));
  if (operation == DifferenceOperation::Since) result = CreateNegatedTemporalDuration(result);
  return Value(result);
}

// AddDurationToInstant
Result<Value> AddDurationToInstant(ArithmeticOperation operation, InstantObject* instant, Value temporalDurationLike) {
  DurationObject* duration = TRY(ToTemporalDuration(temporalDurationLike));
  if (operation == ArithmeticOperation::Subtract) duration = CreateNegatedTemporalDuration(duration);
  Unit largestUnit = DefaultTemporalLargestUnit(duration->fields);
  if (IsDateUnit(largestUnit)) return ThrowRangeError("an Instant cannot add years, months, weeks or days");
  InternalDuration internalDuration = ToInternalDurationRecordWith24HourDays(duration->fields);
  EpochNanoseconds ns = TRY(AddInstant(instant->epochNanoseconds, internalDuration.time));
  return Value(TRY(CreateTemporalInstant(ns)));
}

Result<Value> Instant_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Temporal.Instant requires 'new'");
  BigInt* bigint = TRY(ToBigInt(call.args[0]));
  EpochNanoseconds epochNanoseconds = TRY(ToEpochNanoseconds(bigint));
  return Value(TRY(CreateTemporalInstant(epochNanoseconds, call.newTarget)));
}

Result<Value> Instant_from(NativeCall& call) { return Value(TRY(ToTemporalInstant(call.args[0]))); }

Result<Value> Instant_fromEpochMilliseconds(NativeCall& call) {
  double epochMilliseconds = TRY(ToNumber(call.args[0]));
  // NumberToBigInt
  if (!std::isfinite(epochMilliseconds) || std::trunc(epochMilliseconds) != epochMilliseconds)
    return ThrowRangeError("epoch milliseconds must be an integer");
  if (std::fabs(epochMilliseconds) > 8.64e15) return ThrowRangeError("epoch milliseconds are outside the supported range");
  return Value(TRY(CreateTemporalInstant(EpochNanoseconds(int64_t(epochMilliseconds)) * 1'000'000)));
}

Result<Value> Instant_fromEpochNanoseconds(NativeCall& call) {
  BigInt* bigint = TRY(ToBigInt(call.args[0]));
  EpochNanoseconds epochNanoseconds = TRY(ToEpochNanoseconds(bigint));
  return Value(TRY(CreateTemporalInstant(epochNanoseconds)));
}

Result<Value> Instant_compare(NativeCall& call) {
  InstantObject* one = TRY(ToTemporalInstant(call.args[0]));
  InstantObject* two = TRY(ToTemporalInstant(call.args[1]));
  return num(CompareEpochNanoseconds(one->epochNanoseconds, two->epochNanoseconds));
}

Result<Value> InstantPrototype_epochMilliseconds(NativeCall& call) {
  InstantObject* instant = TRY(thisInstant(call, "get Temporal.Instant.prototype.epochMilliseconds"));
  EpochNanoseconds ns = instant->epochNanoseconds;
  EpochNanoseconds ms = ns / 1'000'000 - (ns % 1'000'000 < 0 ? 1 : 0);
  return num(double(int64_t(ms)));
}

Result<Value> InstantPrototype_epochNanoseconds(NativeCall& call) {
  InstantObject* instant = TRY(thisInstant(call, "get Temporal.Instant.prototype.epochNanoseconds"));
  return Value(EpochNanosecondsToBigInt(instant->epochNanoseconds));
}

Result<Value> InstantPrototype_add(NativeCall& call) {
  InstantObject* instant = TRY(thisInstant(call, "Temporal.Instant.prototype.add"));
  return AddDurationToInstant(ArithmeticOperation::Add, instant, call.args[0]);
}

Result<Value> InstantPrototype_subtract(NativeCall& call) {
  InstantObject* instant = TRY(thisInstant(call, "Temporal.Instant.prototype.subtract"));
  return AddDurationToInstant(ArithmeticOperation::Subtract, instant, call.args[0]);
}

Result<Value> InstantPrototype_until(NativeCall& call) {
  InstantObject* instant = TRY(thisInstant(call, "Temporal.Instant.prototype.until"));
  return DifferenceTemporalInstant(DifferenceOperation::Until, instant, call.args[0], call.args[1]);
}

Result<Value> InstantPrototype_since(NativeCall& call) {
  InstantObject* instant = TRY(thisInstant(call, "Temporal.Instant.prototype.since"));
  return DifferenceTemporalInstant(DifferenceOperation::Since, instant, call.args[0], call.args[1]);
}

Result<Value> InstantPrototype_round(NativeCall& call) {
  InstantObject* instant = TRY(thisInstant(call, "Temporal.Instant.prototype.round"));
  Value roundToValue = call.args[0];
  if (roundToValue.isUndefined()) return ThrowTypeError("Temporal.Instant.prototype.round requires options");
  Object* roundTo;
  if (roundToValue.isString()) {
    roundTo = OrdinaryObjectCreate(nullptr);
    TRY(CreateDataPropertyOrThrow(roundTo, PropertyKey(u"smallestUnit"), roundToValue));
  } else {
    roundTo = TRY(GetOptionsObject(roundToValue));
  }
  int64_t roundingIncrement = TRY(GetRoundingIncrementOption(roundTo));
  RoundingMode roundingMode = TRY(GetRoundingModeOption(roundTo, RoundingMode::HalfExpand));
  Unit smallestUnit = TRY(GetTemporalUnitValuedOption(roundTo, u"smallestUnit", true));
  TRY(ValidateTemporalUnitValue(smallestUnit, UnitGroup::Time));
  int64_t maximum = nsPerDay / UnitLengthInNanoseconds(smallestUnit);
  TRY(ValidateTemporalRoundingIncrement(roundingIncrement, maximum, true));
  EpochNanoseconds roundedNs = RoundTemporalInstant(instant->epochNanoseconds, roundingIncrement, smallestUnit, roundingMode);
  return Value(TRY(CreateTemporalInstant(roundedNs)));
}

Result<Value> InstantPrototype_equals(NativeCall& call) {
  InstantObject* instant = TRY(thisInstant(call, "Temporal.Instant.prototype.equals"));
  InstantObject* other = TRY(ToTemporalInstant(call.args[0]));
  return Value::boolean(instant->epochNanoseconds == other->epochNanoseconds);
}

Result<Value> InstantPrototype_toString(NativeCall& call) {
  InstantObject* instant = TRY(thisInstant(call, "Temporal.Instant.prototype.toString"));
  Object* resolvedOptions = TRY(GetOptionsObject(call.args[0]));
  int digits = TRY(GetTemporalFractionalSecondDigitsOption(resolvedOptions));
  RoundingMode roundingMode = TRY(GetRoundingModeOption(resolvedOptions, RoundingMode::Trunc));
  Unit smallestUnit = TRY(GetTemporalUnitValuedOption(resolvedOptions, u"smallestUnit"));
  Value timeZoneValue = TRY(Get(resolvedOptions, PropertyKey(u"timeZone")));
  TRY(ValidateTemporalUnitValue(smallestUnit, UnitGroup::Time));
  if (smallestUnit == Unit::Hour) return ThrowRangeError("smallestUnit must not be \"hour\"");
  std::optional<TimeZone> timeZone;
  if (!timeZoneValue.isUndefined()) timeZone = TRY(ToTemporalTimeZoneIdentifier(timeZoneValue));
  SecondsStringPrecision precision = TRY(ToSecondsStringPrecisionRecord(smallestUnit, digits));
  EpochNanoseconds roundedNs = RoundTemporalInstant(instant->epochNanoseconds, precision.increment, precision.unit, roundingMode);
  return Value(newString(TemporalInstantToString(roundedNs, timeZone ? &*timeZone : nullptr, precision.precision)));
}

Result<Value> InstantPrototype_toLocaleString(NativeCall& call) {
  InstantObject* instant = TRY(thisInstant(call, "Temporal.Instant.prototype.toLocaleString"));
  return intl::temporalToLocaleString(Value(instant), call.args[0], call.args[1]);
}

Result<Value> InstantPrototype_toJSON(NativeCall& call) {
  InstantObject* instant = TRY(thisInstant(call, "Temporal.Instant.prototype.toJSON"));
  return Value(newString(TemporalInstantToString(instant->epochNanoseconds, nullptr, -1)));
}

Result<Value> InstantPrototype_valueOf(NativeCall&) {
  return ThrowTypeError("Temporal.Instant can't be converted to a primitive: use compare() or epochNanoseconds");
}

Result<Value> InstantPrototype_toZonedDateTimeISO(NativeCall& call) {
  InstantObject* instant = TRY(thisInstant(call, "Temporal.Instant.prototype.toZonedDateTimeISO"));
  TimeZone timeZone = TRY(ToTemporalTimeZoneIdentifier(call.args[0]));
  return Value(TRY(CreateTemporalZonedDateTime(instant->epochNanoseconds, timeZone, IsoCalendar())));
}

void installInstant(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor = defineConstructor(realm, Intrinsic::TemporalInstant, Instant_construct, 1, PropertyKey(u"Instant"),
                                            proto, Intrinsic::TemporalInstantPrototype, false);
  defineValue(TemporalNamespace(realm), PropertyKey(u"Instant"), Value(ctor));
  defineFunction(realm, ctor, PropertyKey(u"from"), 1, Instant_from);
  defineFunction(realm, ctor, PropertyKey(u"fromEpochMilliseconds"), 1, Instant_fromEpochMilliseconds);
  defineFunction(realm, ctor, PropertyKey(u"fromEpochNanoseconds"), 1, Instant_fromEpochNanoseconds);
  defineFunction(realm, ctor, PropertyKey(u"compare"), 2, Instant_compare);

  defineAccessor(realm, proto, PropertyKey(u"epochMilliseconds"), InstantPrototype_epochMilliseconds, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"epochNanoseconds"), InstantPrototype_epochNanoseconds, nullptr);
  defineFunction(realm, proto, PropertyKey(u"add"), 1, InstantPrototype_add);
  defineFunction(realm, proto, PropertyKey(u"subtract"), 1, InstantPrototype_subtract);
  defineFunction(realm, proto, PropertyKey(u"until"), 1, InstantPrototype_until);
  defineFunction(realm, proto, PropertyKey(u"since"), 1, InstantPrototype_since);
  defineFunction(realm, proto, PropertyKey(u"round"), 1, InstantPrototype_round);
  defineFunction(realm, proto, PropertyKey(u"equals"), 1, InstantPrototype_equals);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, InstantPrototype_toString);
  defineFunction(realm, proto, PropertyKey(u"toLocaleString"), 0, InstantPrototype_toLocaleString);
  defineFunction(realm, proto, PropertyKey(u"toJSON"), 0, InstantPrototype_toJSON);
  defineFunction(realm, proto, PropertyKey(u"valueOf"), 0, InstantPrototype_valueOf);
  defineFunction(realm, proto, PropertyKey(u"toZonedDateTimeISO"), 1, InstantPrototype_toZonedDateTimeISO);
  defineValue(proto, PropertyKey(builtins::symbols().toStringTag), str(u"Temporal.Instant"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(TemporalInstant, kOrderDefault, installInstant);

}  // namespace snowman::temporal
