// Temporal option getters (GetOptionsObject, GetOption and the GetTemporal…Option family).
#include <cmath>

#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

Result<Object*> GetOptionsObject(Value options) {
  if (options.isUndefined()) return OrdinaryObjectCreate(nullptr);
  if (options.isObject()) return options.asObject();
  return ThrowTypeError("options must be an object or undefined");
}

Result<std::optional<std::u16string>> GetStringOption(Object* options, std::u16string_view property,
                                                       std::initializer_list<std::u16string_view> values) {
  Value value = TRY(Get(options, PropertyKey(property)));
  if (value.isUndefined()) return std::optional<std::u16string>();
  String* s = TRY(ToString(value));
  std::u16string_view v = s->view();
  if (values.size()) {
    bool found = false;
    for (auto allowed : values)
      if (allowed == v) found = true;
    if (!found) return ThrowRangeError(std::string(utf16ToUtf8(v)) + " is not a valid value for " + utf16ToUtf8(property));
  }
  return std::optional<std::u16string>(std::u16string(v));
}

Result<Overflow> GetTemporalOverflowOption(Object* options) {
  auto v = TRY(GetStringOption(options, u"overflow", {u"constrain", u"reject"}));
  return (!v || *v == u"constrain") ? Overflow::Constrain : Overflow::Reject;
}

Result<Disambiguation> GetTemporalDisambiguationOption(Object* options) {
  auto v = TRY(GetStringOption(options, u"disambiguation", {u"compatible", u"earlier", u"later", u"reject"}));
  if (!v || *v == u"compatible") return Disambiguation::Compatible;
  if (*v == u"earlier") return Disambiguation::Earlier;
  if (*v == u"later") return Disambiguation::Later;
  return Disambiguation::Reject;
}

Result<OffsetOption> GetTemporalOffsetOption(Object* options, OffsetOption fallback) {
  auto v = TRY(GetStringOption(options, u"offset", {u"prefer", u"use", u"ignore", u"reject"}));
  if (!v) return fallback;
  if (*v == u"prefer") return OffsetOption::Prefer;
  if (*v == u"use") return OffsetOption::Use;
  if (*v == u"ignore") return OffsetOption::Ignore;
  return OffsetOption::Reject;
}

Result<ShowCalendar> GetTemporalShowCalendarNameOption(Object* options) {
  auto v = TRY(GetStringOption(options, u"calendarName", {u"auto", u"always", u"never", u"critical"}));
  if (!v || *v == u"auto") return ShowCalendar::Auto;
  if (*v == u"always") return ShowCalendar::Always;
  if (*v == u"never") return ShowCalendar::Never;
  return ShowCalendar::Critical;
}

Result<ShowTimeZone> GetTemporalShowTimeZoneNameOption(Object* options) {
  auto v = TRY(GetStringOption(options, u"timeZoneName", {u"auto", u"never", u"critical"}));
  if (!v || *v == u"auto") return ShowTimeZone::Auto;
  if (*v == u"never") return ShowTimeZone::Never;
  return ShowTimeZone::Critical;
}

Result<ShowOffset> GetTemporalShowOffsetOption(Object* options) {
  auto v = TRY(GetStringOption(options, u"offset", {u"auto", u"never"}));
  return (!v || *v == u"auto") ? ShowOffset::Auto : ShowOffset::Never;
}

Result<int> GetTemporalFractionalSecondDigitsOption(Object* options) {
  Value digitsValue = TRY(Get(options, PropertyKey(u"fractionalSecondDigits")));
  if (digitsValue.isUndefined()) return -1;
  if (!digitsValue.isNumber()) {
    String* s = TRY(ToString(digitsValue));
    if (s->view() != u"auto") return ThrowRangeError("fractionalSecondDigits must be 'auto' or 0 through 9");
    return -1;
  }
  double d = digitsValue.asNumber();
  if (!std::isfinite(d)) return ThrowRangeError("fractionalSecondDigits must be 'auto' or 0 through 9");
  double digitCount = std::floor(d);
  if (digitCount < 0 || digitCount > 9) return ThrowRangeError("fractionalSecondDigits must be 'auto' or 0 through 9");
  return int(digitCount);
}

Result<RoundingMode> GetRoundingModeOption(Object* options, RoundingMode fallback) {
  auto v = TRY(GetStringOption(options, u"roundingMode",
                               {u"ceil", u"floor", u"expand", u"trunc", u"halfCeil", u"halfFloor", u"halfExpand",
                                u"halfTrunc", u"halfEven"}));
  if (!v) return fallback;
  static const std::pair<const char16_t*, RoundingMode> modes[] = {
      {u"ceil", RoundingMode::Ceil},         {u"floor", RoundingMode::Floor},         {u"expand", RoundingMode::Expand},
      {u"trunc", RoundingMode::Trunc},       {u"halfCeil", RoundingMode::HalfCeil},   {u"halfFloor", RoundingMode::HalfFloor},
      {u"halfExpand", RoundingMode::HalfExpand}, {u"halfTrunc", RoundingMode::HalfTrunc}, {u"halfEven", RoundingMode::HalfEven}};
  for (auto& [name, mode] : modes)
    if (*v == name) return mode;
  return fallback;
}

Result<int64_t> GetRoundingIncrementOption(Object* options) {
  Value value = TRY(Get(options, PropertyKey(u"roundingIncrement")));
  if (value.isUndefined()) return int64_t(1);
  double integerIncrement = TRY(ToIntegerWithTruncation(value));
  if (integerIncrement < 1 || integerIncrement > 1e9) return ThrowRangeError("roundingIncrement is out of range");
  return int64_t(integerIncrement);
}

Result<void> ValidateTemporalRoundingIncrement(int64_t increment, int64_t dividend, bool inclusive) {
  int64_t maximum = inclusive ? dividend : dividend - 1;
  if (increment > maximum) return ThrowRangeError("roundingIncrement is out of range");
  if (dividend % increment != 0) return ThrowRangeError("roundingIncrement does not divide evenly");
  return {};
}

Result<Unit> GetTemporalUnitValuedOption(Object* options, std::u16string_view key, bool required) {
  static const std::pair<const char16_t*, Unit> names[] = {
      {u"year", Unit::Year},
      {u"years", Unit::Year},
      {u"month", Unit::Month},
      {u"months", Unit::Month},
      {u"week", Unit::Week},
      {u"weeks", Unit::Week},
      {u"day", Unit::Day},
      {u"days", Unit::Day},
      {u"hour", Unit::Hour},
      {u"hours", Unit::Hour},
      {u"minute", Unit::Minute},
      {u"minutes", Unit::Minute},
      {u"second", Unit::Second},
      {u"seconds", Unit::Second},
      {u"millisecond", Unit::Millisecond},
      {u"milliseconds", Unit::Millisecond},
      {u"microsecond", Unit::Microsecond},
      {u"microseconds", Unit::Microsecond},
      {u"nanosecond", Unit::Nanosecond},
      {u"nanoseconds", Unit::Nanosecond},
      {u"auto", Unit::Auto},
  };
  Value value = TRY(Get(options, PropertyKey(key)));
  if (value.isUndefined()) {
    if (required) return ThrowRangeError(utf16ToUtf8(key) + " is required");
    return Unit::Unset;
  }
  String* s = TRY(ToString(value));
  for (auto& [name, unit] : names)
    if (s->view() == name) return unit;
  return ThrowRangeError(s->toUtf8() + " is not a valid value for " + utf16ToUtf8(key));
}

Result<void> ValidateTemporalUnitValue(Unit value, UnitGroup unitGroup, bool allowAuto, std::optional<Unit> extraUnit) {
  if (value == Unit::Unset) return {};
  if (allowAuto && value == Unit::Auto) return {};
  if (extraUnit && *extraUnit == value) return {};
  if (value == Unit::Auto) return ThrowRangeError("'auto' is not allowed here");
  bool date = IsDateUnit(value);
  if (date && (unitGroup == UnitGroup::Date || unitGroup == UnitGroup::DateTime)) return {};
  if (!date && (unitGroup == UnitGroup::Time || unitGroup == UnitGroup::DateTime)) return {};
  return ThrowRangeError(utf16ToUtf8(UnitSingularName(value)) + " is not a valid unit here");
}

int64_t MaximumTemporalDurationRoundingIncrement(Unit unit) {
  switch (unit) {
    case Unit::Year:
    case Unit::Month:
    case Unit::Week:
    case Unit::Day: return 0;
    case Unit::Hour: return 24;
    case Unit::Minute:
    case Unit::Second: return 60;
    default: return 1000;
  }
}

Result<DifferenceSettings> GetDifferenceSettings(DifferenceOperation operation, Object* options, UnitGroup unitGroup,
                                                 uint32_t disallowedUnits, Unit fallbackSmallestUnit,
                                                 Unit smallestLargestDefaultUnit) {
  Unit largestUnit = TRY(GetTemporalUnitValuedOption(options, u"largestUnit"));
  int64_t roundingIncrement = TRY(GetRoundingIncrementOption(options));
  RoundingMode roundingMode = TRY(GetRoundingModeOption(options, RoundingMode::Trunc));
  Unit smallestUnit = TRY(GetTemporalUnitValuedOption(options, u"smallestUnit"));
  TRY(ValidateTemporalUnitValue(largestUnit, unitGroup, true));
  if (largestUnit == Unit::Unset) largestUnit = Unit::Auto;
  if (largestUnit != Unit::Auto && (disallowedUnits & unitBit(largestUnit)))
    return ThrowRangeError(utf16ToUtf8(UnitSingularName(largestUnit)) + " is not allowed as largestUnit");
  TRY(ValidateTemporalUnitValue(smallestUnit, unitGroup));
  if (smallestUnit == Unit::Unset) smallestUnit = fallbackSmallestUnit;
  if (disallowedUnits & unitBit(smallestUnit))
    return ThrowRangeError(utf16ToUtf8(UnitSingularName(smallestUnit)) + " is not allowed as smallestUnit");
  Unit defaultLargestUnit = LargerOfTwoTemporalUnits(smallestLargestDefaultUnit, smallestUnit);
  if (largestUnit == Unit::Auto) largestUnit = defaultLargestUnit;
  if (LargerOfTwoTemporalUnits(largestUnit, smallestUnit) != largestUnit)
    return ThrowRangeError("largestUnit must be larger than smallestUnit");
  int64_t maximum = MaximumTemporalDurationRoundingIncrement(smallestUnit);
  if (maximum) TRY(ValidateTemporalRoundingIncrement(roundingIncrement, maximum, false));
  if (operation == DifferenceOperation::Since) roundingMode = NegateRoundingMode(roundingMode);
  return DifferenceSettings{smallestUnit, largestUnit, roundingMode, roundingIncrement};
}

Result<SecondsStringPrecision> ToSecondsStringPrecisionRecord(Unit smallestUnit, int fractionalDigitCount) {
  switch (smallestUnit) {
    case Unit::Minute: return SecondsStringPrecision{-2, Unit::Minute, 1};
    case Unit::Second: return SecondsStringPrecision{0, Unit::Second, 1};
    case Unit::Millisecond: return SecondsStringPrecision{3, Unit::Millisecond, 1};
    case Unit::Microsecond: return SecondsStringPrecision{6, Unit::Microsecond, 1};
    case Unit::Nanosecond: return SecondsStringPrecision{9, Unit::Nanosecond, 1};
    case Unit::Unset: break;
    default: return ThrowRangeError("smallestUnit is not a valid unit here");
  }
  if (fractionalDigitCount == -1) return SecondsStringPrecision{-1, Unit::Nanosecond, 1};
  if (fractionalDigitCount == 0) return SecondsStringPrecision{0, Unit::Second, 1};
  static const int64_t pow10[] = {1, 10, 100};
  if (fractionalDigitCount <= 3) return SecondsStringPrecision{fractionalDigitCount, Unit::Millisecond, pow10[3 - fractionalDigitCount]};
  if (fractionalDigitCount <= 6) return SecondsStringPrecision{fractionalDigitCount, Unit::Microsecond, pow10[6 - fractionalDigitCount]};
  return SecondsStringPrecision{fractionalDigitCount, Unit::Nanosecond, pow10[9 - fractionalDigitCount]};
}

Result<Overflow> GetTemporalOverflowOptionFrom(Value options) {
  Object* resolvedOptions = TRY(GetOptionsObject(options));
  return GetTemporalOverflowOption(resolvedOptions);
}

Result<Object*> GetRoundingOptionsArgument(Value argument, const char16_t* key, const char* method) {
  if (argument.isUndefined()) return ThrowTypeError(std::string(method) + " requires an options argument");
  if (argument.isString()) {
    Object* o = OrdinaryObjectCreate(nullptr);
    TRY(CreateDataPropertyOrThrow(o, PropertyKey(key), argument));
    return o;
  }
  return GetOptionsObject(argument);
}

Result<bool> IsPartialTemporalObject(Value value) {
  if (!value.isObject()) return false;
  Object* o = value.asObject();
  if (o->is<PlainDateObject>() || o->is<PlainDateTimeObject>() || o->is<PlainMonthDayObject>() || o->is<PlainTimeObject>() ||
      o->is<PlainYearMonthObject>() || o->is<ZonedDateTimeObject>())
    return false;
  Value calendarProperty = TRY(Get(o, PropertyKey(u"calendar")));
  if (!calendarProperty.isUndefined()) return false;
  Value timeZoneProperty = TRY(Get(o, PropertyKey(u"timeZone")));
  if (!timeZoneProperty.isUndefined()) return false;
  return true;
}

}  // namespace snowman::temporal
