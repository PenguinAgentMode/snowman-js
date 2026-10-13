// Temporal.ZonedDateTime (proposal-temporal §6) and the time zone operations temporal.h declares for the core.
#include <cmath>

#include "builtins/temporal/temporal.h"
#include "runtime/bigint.h"

namespace snowman::temporal {

using builtins::num;
using builtins::str;

namespace {

// ParseTemporalTimeZoneString
Result<TimeZoneIdentifierParseRecord> ParseTemporalTimeZoneString(std::u16string_view timeZoneString) {
  if (auto record = ParseTimeZoneIdentifier(timeZoneString)) return *record;
  ParsedISODateTime result = TRY(ParseISODateTime(timeZoneString, kAllFormats));
  const ParsedTimeZoneInfo& timeZoneResult = result.timeZone;
  if (timeZoneResult.annotation) {
    if (auto record = ParseTimeZoneIdentifier(*timeZoneResult.annotation)) return *record;
    return ThrowRangeError("invalid time zone annotation");
  }
  if (timeZoneResult.z) return TimeZoneIdentifierParseRecord{u"UTC", std::nullopt};
  if (timeZoneResult.offsetString) {
    if (auto record = ParseTimeZoneIdentifier(*timeZoneResult.offsetString)) return *record;
    return ThrowRangeError("a UTC offset time zone must not have sub-minute precision");
  }
  return ThrowRangeError("the string has no time zone");
}

}  // namespace

Result<TimeZone> ToTemporalTimeZoneIdentifier(Value temporalTimeZoneLike) {
  if (auto* zoned = asTemporal<ZonedDateTimeObject>(temporalTimeZoneLike)) return zoned->timeZone;
  if (!temporalTimeZoneLike.isString()) return ThrowTypeError("a time zone must be a string");
  TimeZoneIdentifierParseRecord parseResult = TRY(ParseTemporalTimeZoneString(temporalTimeZoneLike.asString()->view()));
  return TimeZoneFromParseRecord(parseResult);
}

// The adapters the core calls (temporal.h).
Result<int64_t> GetOffsetNanosecondsForZone(const TimeZone& timeZone, EpochNanoseconds epochNs) {
  return GetOffsetNanosecondsFor(timeZone, epochNs);
}
Result<EpochNanoseconds> GetEpochNanosecondsForZone(const TimeZone& timeZone, const ISODateTime& isoDateTime,
                                                    Disambiguation disambiguation) {
  return GetEpochNanosecondsFor(timeZone, isoDateTime, disambiguation);
}
Result<EpochNanoseconds> GetStartOfDayForZone(const TimeZone& timeZone, const ISODate& isoDate) {
  return GetStartOfDay(timeZone, isoDate);
}
Result<ISODateTime> GetISODateTimeForZone(const TimeZone& timeZone, EpochNanoseconds epochNs) {
  return GetISODateTimeFor(timeZone, epochNs);
}

Result<ZonedDateTimeObject*> CreateTemporalZonedDateTime(EpochNanoseconds epochNanoseconds, const TimeZone& timeZone,
                                                         const CalendarId& calendar, Object* newTarget) {
  Object* prototype = newTarget ? TRY(GetPrototypeFromConstructor(newTarget, Intrinsic::TemporalZonedDateTimePrototype))
                                : builtins::intrinsic(Intrinsic::TemporalZonedDateTimePrototype);
  return make<ZonedDateTimeObject>(prototype, epochNanoseconds, timeZone, calendar);
}

// ---- abstract operations ----------------------------------------------------------------------------------------

namespace {

// CheckISODaysRange
bool ISODaysInRange(const ISODate& date) {
  int64_t days = ISODateToEpochDays(date.year, date.month - 1, date.day);
  return days >= -100'000'000 && days <= 100'000'000;
}

// AddInstant
Result<EpochNanoseconds> AddInstant(EpochNanoseconds epochNanoseconds, TimeDuration timeDuration) {
  EpochNanoseconds result = AddTimeDurationToEpochNanoseconds(timeDuration, epochNanoseconds);
  if (!IsValidEpochNanoseconds(result)) return ThrowRangeError("date-time is outside the supported range");
  return result;
}

// DifferenceInstant
Result<InternalDuration> DifferenceInstant(EpochNanoseconds ns1, EpochNanoseconds ns2, int64_t roundingIncrement,
                                           Unit smallestUnit, RoundingMode roundingMode) {
  TimeDuration timeDuration = TimeDurationFromEpochNanosecondsDifference(ns2, ns1);
  timeDuration = TRY(RoundTimeDuration(timeDuration, roundingIncrement, smallestUnit, roundingMode));
  return CombineDateAndTimeDuration(DateDuration{}, timeDuration);
}

// RoundTemporalInstant
EpochNanoseconds RoundTemporalInstant(EpochNanoseconds ns, int64_t increment, Unit unit, RoundingMode roundingMode) {
  return RoundNumberToIncrementAsIfPositive(ns, Int128(increment) * UnitLengthInNanoseconds(unit), roundingMode);
}

// DifferenceZonedDateTime
Result<InternalDuration> DifferenceZonedDateTime(EpochNanoseconds ns1, EpochNanoseconds ns2, const TimeZone& timeZone,
                                                 const CalendarId& calendar, Unit largestUnit) {
  if (ns1 == ns2) return CombineDateAndTimeDuration(DateDuration{}, 0);
  ISODateTime startDateTime = GetISODateTimeFor(timeZone, ns1);
  ISODateTime endDateTime = GetISODateTimeFor(timeZone, ns2);
  if (CompareISODate(startDateTime.isoDate, endDateTime.isoDate) == 0) {
    return CombineDateAndTimeDuration(DateDuration{}, TimeDurationFromEpochNanosecondsDifference(ns2, ns1));
  }
  const int sign = ns2 - ns1 < 0 ? 1 : -1;
  const int maxDayCorrection = sign == -1 ? 2 : 1;
  int dayCorrection = 0;
  TimeDuration timeDuration = DifferenceTime(startDateTime.time, endDateTime.time);
  if (TimeDurationSign(timeDuration) == sign) dayCorrection++;
  bool success = false;
  ISODateTime intermediateDateTime;
  while (dayCorrection <= maxDayCorrection && !success) {
    ISODate intermediateDate = AddDaysToISODate(endDateTime.isoDate, int64_t(dayCorrection) * sign);
    intermediateDateTime = CombineISODateAndTimeRecord(intermediateDate, startDateTime.time);
    EpochNanoseconds intermediateNs = TRY(GetEpochNanosecondsFor(timeZone, intermediateDateTime, Disambiguation::Compatible));
    timeDuration = TimeDurationFromEpochNanosecondsDifference(ns2, intermediateNs);
    if (sign != TimeDurationSign(timeDuration)) success = true;
    dayCorrection++;
  }
  Unit dateLargestUnit = LargerOfTwoTemporalUnits(largestUnit, Unit::Day);
  DateDuration dateDifference = CalendarDateUntil(calendar, startDateTime.isoDate, intermediateDateTime.isoDate, dateLargestUnit);
  return CombineDateAndTimeDuration(dateDifference, timeDuration);
}

}  // namespace

Result<EpochNanoseconds> InterpretISODateTimeOffset(const ISODate& isoDate, const std::optional<TimeRecord>& time,
                                                    OffsetBehaviour offsetBehaviour, int64_t offsetNanoseconds,
                                                    const TimeZone& timeZone, Disambiguation disambiguation,
                                                    OffsetOption offsetOption, MatchBehaviour matchBehaviour) {
  if (!time) return GetStartOfDay(timeZone, isoDate);
  ISODateTime isoDateTime = CombineISODateAndTimeRecord(isoDate, *time);
  if (offsetBehaviour == OffsetBehaviour::Wall ||
      (offsetBehaviour == OffsetBehaviour::Option && offsetOption == OffsetOption::Ignore)) {
    return GetEpochNanosecondsFor(timeZone, isoDateTime, disambiguation);
  }
  if (offsetBehaviour == OffsetBehaviour::Exact ||
      (offsetBehaviour == OffsetBehaviour::Option && offsetOption == OffsetOption::Use)) {
    ISODateTime balanced = BalanceISODateTime(isoDate, time->hour, time->minute, time->second, time->millisecond,
                                              time->microsecond, Int128(time->nanosecond) - offsetNanoseconds);
    if (!ISODaysInRange(balanced.isoDate)) return ThrowRangeError("date-time is outside the supported range");
    EpochNanoseconds epochNanoseconds = GetUTCEpochNanoseconds(balanced);
    if (!IsValidEpochNanoseconds(epochNanoseconds)) return ThrowRangeError("date-time is outside the supported range");
    return epochNanoseconds;
  }
  if (!ISODaysInRange(isoDate)) return ThrowRangeError("date-time is outside the supported range");
  EpochNanoseconds utcEpochNanoseconds = GetUTCEpochNanoseconds(isoDateTime);
  std::vector<EpochNanoseconds> possibleEpochNs = TRY(GetPossibleEpochNanoseconds(timeZone, isoDateTime));
  for (EpochNanoseconds candidate : possibleEpochNs) {
    Int128 candidateOffset = utcEpochNanoseconds - candidate;
    if (candidateOffset == offsetNanoseconds) return candidate;
    if (matchBehaviour == MatchBehaviour::MatchMinutes) {
      Int128 rounded = RoundNumberToIncrement(candidateOffset, nsPerMinute, RoundingMode::HalfExpand);
      if (rounded == offsetNanoseconds) return candidate;
    }
  }
  if (offsetOption == OffsetOption::Reject) return ThrowRangeError("the UTC offset is not valid for this time zone");
  return DisambiguatePossibleEpochNanoseconds(possibleEpochNs, timeZone, isoDateTime, disambiguation);
}

Result<EpochNanoseconds> AddZonedDateTime(EpochNanoseconds epochNanoseconds, const TimeZone& timeZone,
                                          const CalendarId& calendar, const InternalDuration& duration, Overflow overflow) {
  if (DateDurationSign(duration.date) == 0) return AddInstant(epochNanoseconds, duration.time);
  ISODateTime isoDateTime = GetISODateTimeFor(timeZone, epochNanoseconds);
  ISODate addedDate = TRY(CalendarDateAdd(calendar, isoDateTime.isoDate, duration.date, overflow));
  ISODateTime intermediateDateTime = CombineISODateAndTimeRecord(addedDate, isoDateTime.time);
  if (!ISODateTimeWithinLimits(intermediateDateTime)) return ThrowRangeError("date-time is outside the supported range");
  EpochNanoseconds intermediateNs = TRY(GetEpochNanosecondsFor(timeZone, intermediateDateTime, Disambiguation::Compatible));
  return AddInstant(intermediateNs, duration.time);
}

Result<InternalDuration> DifferenceZonedDateTimeWithRounding(EpochNanoseconds ns1, EpochNanoseconds ns2,
                                                             const TimeZone& timeZone, const CalendarId& calendar,
                                                             Unit largestUnit, int64_t roundingIncrement,
                                                             Unit smallestUnit, RoundingMode roundingMode) {
  if (!IsDateUnit(largestUnit)) return DifferenceInstant(ns1, ns2, roundingIncrement, smallestUnit, roundingMode);
  InternalDuration difference = TRY(DifferenceZonedDateTime(ns1, ns2, timeZone, calendar, largestUnit));
  if (smallestUnit == Unit::Nanosecond && roundingIncrement == 1) return difference;
  ISODateTime dateTime = GetISODateTimeFor(timeZone, ns1);
  return RoundRelativeDuration(difference, ns1, ns2, dateTime, &timeZone, calendar, largestUnit, roundingIncrement,
                               smallestUnit, roundingMode);
}

Result<double> DifferenceZonedDateTimeWithTotal(EpochNanoseconds ns1, EpochNanoseconds ns2, const TimeZone& timeZone,
                                                const CalendarId& calendar, Unit unit) {
  if (!IsDateUnit(unit)) return TotalTimeDuration(TimeDurationFromEpochNanosecondsDifference(ns2, ns1), unit);
  InternalDuration difference = TRY(DifferenceZonedDateTime(ns1, ns2, timeZone, calendar, unit));
  ISODateTime dateTime = GetISODateTimeFor(timeZone, ns1);
  return TotalRelativeDuration(difference, ns1, ns2, dateTime, &timeZone, calendar, unit);
}

namespace {

// ToTemporalZonedDateTime
Result<ZonedDateTimeObject*> ToTemporalZonedDateTime(Value item, Value options = Value::undefined()) {
  bool hasUTCDesignator = false;
  MatchBehaviour matchBehaviour = MatchBehaviour::MatchExactly;
  TimeZone timeZone;
  std::optional<std::u16string> offsetString;
  CalendarId calendar;
  Disambiguation disambiguation;
  OffsetOption offsetOption;
  ISODate isoDate;
  std::optional<TimeRecord> time;
  if (item.isObject()) {
    if (auto* zoned = asTemporal<ZonedDateTimeObject>(item)) {
      Object* resolvedOptions = TRY(GetOptionsObject(options));
      TRY(GetTemporalDisambiguationOption(resolvedOptions));
      TRY(GetTemporalOffsetOption(resolvedOptions, OffsetOption::Reject));
      TRY(GetTemporalOverflowOption(resolvedOptions));
      return CreateTemporalZonedDateTime(zoned->epochNanoseconds, zoned->timeZone, zoned->calendar);
    }
    calendar = TRY(GetTemporalCalendarIdentifierWithISODefault(item.asObject()));
    CalendarFields fields = TRY(PrepareCalendarFields(calendar, item.asObject(), kFieldsDate,
                                                      kFieldsTime | kFieldOffset | kFieldTimeZone, kFieldTimeZone));
    timeZone = *fields.timeZone;
    offsetString = fields.offset;
    Object* resolvedOptions = TRY(GetOptionsObject(options));
    disambiguation = TRY(GetTemporalDisambiguationOption(resolvedOptions));
    offsetOption = TRY(GetTemporalOffsetOption(resolvedOptions, OffsetOption::Reject));
    Overflow overflow = TRY(GetTemporalOverflowOption(resolvedOptions));
    ISODateTime result = TRY(InterpretTemporalDateTimeFields(calendar, fields, overflow));
    isoDate = result.isoDate;
    time = result.time;
  } else {
    if (!item.isString()) return ThrowTypeError("a ZonedDateTime must be a Temporal object or an ISO string");
    ParsedISODateTime result = TRY(ParseISODateTime(item.asString()->view(), kDateTimeZoned));
    timeZone = TRY(ToTemporalTimeZoneIdentifier(Value(newString(*result.timeZone.annotation))));
    offsetString = result.timeZone.offsetString;
    if (result.timeZone.z) hasUTCDesignator = true;
    calendar = TRY(CanonicalizeCalendar(result.calendar ? *result.calendar : IsoCalendar()));
    matchBehaviour = MatchBehaviour::MatchMinutes;
    if (offsetString && HasSubMinutePrecision(*offsetString)) matchBehaviour = MatchBehaviour::MatchExactly;
    Object* resolvedOptions = TRY(GetOptionsObject(options));
    disambiguation = TRY(GetTemporalDisambiguationOption(resolvedOptions));
    offsetOption = TRY(GetTemporalOffsetOption(resolvedOptions, OffsetOption::Reject));
    TRY(GetTemporalOverflowOption(resolvedOptions));
    isoDate = CreateISODateRecord(*result.year, result.month, result.day);
    time = result.time;
  }
  OffsetBehaviour offsetBehaviour = hasUTCDesignator ? OffsetBehaviour::Exact
                                    : !offsetString  ? OffsetBehaviour::Wall
                                                     : OffsetBehaviour::Option;
  int64_t offsetNanoseconds = 0;
  if (offsetBehaviour == OffsetBehaviour::Option) offsetNanoseconds = TRY(ParseDateTimeUTCOffset(*offsetString));
  EpochNanoseconds epochNanoseconds = TRY(InterpretISODateTimeOffset(isoDate, time, offsetBehaviour, offsetNanoseconds, timeZone,
                                                                     disambiguation, offsetOption, matchBehaviour));
  return CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar);
}

// TemporalZonedDateTimeToString
std::u16string TemporalZonedDateTimeToString(const ZonedDateTimeObject* zonedDateTime, int precision, ShowCalendar showCalendar,
                                             ShowTimeZone showTimeZone, ShowOffset showOffset, int64_t increment = 1,
                                             Unit unit = Unit::Nanosecond, RoundingMode roundingMode = RoundingMode::Trunc) {
  EpochNanoseconds epochNs = RoundTemporalInstant(zonedDateTime->epochNanoseconds, increment, unit, roundingMode);
  const TimeZone& timeZone = zonedDateTime->timeZone;
  int64_t offsetNanoseconds = GetOffsetNanosecondsFor(timeZone, epochNs);
  ISODateTime isoDateTime = GetISODateTimeFor(timeZone, epochNs);
  std::u16string result = ISODateTimeToString(isoDateTime, IsoCalendar(), precision, ShowCalendar::Never);
  if (showOffset != ShowOffset::Never) result += FormatDateTimeUTCOffsetRounded(offsetNanoseconds);
  if (showTimeZone != ShowTimeZone::Never) {
    result += showTimeZone == ShowTimeZone::Critical ? u"[!" : u"[";
    result += TimeZoneIdentifier(timeZone);
    result += u"]";
  }
  return result + FormatCalendarAnnotation(zonedDateTime->calendar, showCalendar);
}

// DifferenceTemporalZonedDateTime
Result<Value> DifferenceTemporalZonedDateTime(DifferenceOperation operation, ZonedDateTimeObject* zonedDateTime,
                                              Value otherValue, Value options) {
  ZonedDateTimeObject* other = TRY(ToTemporalZonedDateTime(otherValue));
  if (zonedDateTime->calendar != other->calendar) return ThrowRangeError("cannot compute the difference between calendars");
  Object* resolvedOptions = TRY(GetOptionsObject(options));
  DifferenceSettings settings =
      TRY(GetDifferenceSettings(operation, resolvedOptions, UnitGroup::DateTime, 0, Unit::Nanosecond, Unit::Hour));
  DurationObject* result;
  if (!IsDateUnit(settings.largestUnit)) {
    InternalDuration internalDuration = TRY(DifferenceInstant(zonedDateTime->epochNanoseconds, other->epochNanoseconds,
                                                              settings.roundingIncrement, settings.smallestUnit,
                                                              settings.roundingMode));
    result = TRY(TemporalDurationFromInternal(internalDuration, settings.largestUnit));
  } else {
    if (!TimeZoneEquals(zonedDateTime->timeZone, other->timeZone)) {
      return ThrowRangeError("date units can only be computed between ZonedDateTimes in the same time zone");
    }
    if (zonedDateTime->epochNanoseconds == other->epochNanoseconds) return Value(TRY(CreateTemporalDuration(DurationFields{})));
    InternalDuration internalDuration = TRY(DifferenceZonedDateTimeWithRounding(
        zonedDateTime->epochNanoseconds, other->epochNanoseconds, zonedDateTime->timeZone, zonedDateTime->calendar,
        settings.largestUnit, settings.roundingIncrement, settings.smallestUnit, settings.roundingMode));
    result = TRY(TemporalDurationFromInternal(internalDuration, Unit::Hour));
  }
  if (operation == DifferenceOperation::Since) result = CreateNegatedTemporalDuration(result);
  return Value(result);
}

// AddDurationToZonedDateTime
Result<Value> AddDurationToZonedDateTime(ArithmeticOperation operation, ZonedDateTimeObject* zonedDateTime,
                                         Value temporalDurationLike, Value options) {
  DurationObject* duration = TRY(ToTemporalDuration(temporalDurationLike));
  if (operation == ArithmeticOperation::Subtract) duration = CreateNegatedTemporalDuration(duration);
  Object* resolvedOptions = TRY(GetOptionsObject(options));
  Overflow overflow = TRY(GetTemporalOverflowOption(resolvedOptions));
  InternalDuration internalDuration = ToInternalDurationRecord(duration->fields);
  EpochNanoseconds epochNanoseconds = TRY(AddZonedDateTime(zonedDateTime->epochNanoseconds, zonedDateTime->timeZone,
                                                           zonedDateTime->calendar, internalDuration, overflow));
  return Value(TRY(CreateTemporalZonedDateTime(epochNanoseconds, zonedDateTime->timeZone, zonedDateTime->calendar)));
}

// ---- the constructor and its functions ----------------------------------------------------------------------------

Result<ZonedDateTimeObject*> thisZoned(NativeCall& call, const char* method) {
  return thisTemporal<ZonedDateTimeObject>(call.thisValue, method);
}

ISODateTime isoDateTimeOf(const ZonedDateTimeObject* zoned) {
  return GetISODateTimeFor(zoned->timeZone, zoned->epochNanoseconds);
}

Result<Value> ZonedDateTime_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Temporal.ZonedDateTime requires 'new'");
  BigInt* bigint = TRY(ToBigInt(call.args[0]));
  std::optional<EpochNanoseconds> epochNanoseconds = BigIntToEpochNanoseconds(bigint);
  if (!epochNanoseconds || !IsValidEpochNanoseconds(*epochNanoseconds)) {
    return ThrowRangeError("epoch nanoseconds are outside the supported range");
  }
  Value timeZoneValue = call.args[1];
  if (!timeZoneValue.isString()) return ThrowTypeError("the time zone must be a string");
  auto timeZoneParse = ParseTimeZoneIdentifier(timeZoneValue.asString()->view());
  if (!timeZoneParse) return ThrowRangeError("invalid time zone identifier");
  TimeZone timeZone = TRY(TimeZoneFromParseRecord(*timeZoneParse));
  Value calendarValue = call.args[2];
  CalendarId calendar = IsoCalendar();
  if (!calendarValue.isUndefined()) {
    if (!calendarValue.isString()) return ThrowTypeError("the calendar must be a string");
    calendar = TRY(CanonicalizeCalendar(calendarValue.asString()->view()));
  }
  return Value(TRY(CreateTemporalZonedDateTime(*epochNanoseconds, timeZone, calendar, call.newTarget)));
}

Result<Value> ZonedDateTime_from(NativeCall& call) { return Value(TRY(ToTemporalZonedDateTime(call.args[0], call.args[1]))); }

Result<Value> ZonedDateTime_compare(NativeCall& call) {
  ZonedDateTimeObject* one = TRY(ToTemporalZonedDateTime(call.args[0]));
  ZonedDateTimeObject* two = TRY(ToTemporalZonedDateTime(call.args[1]));
  EpochNanoseconds a = one->epochNanoseconds, b = two->epochNanoseconds;
  return num(a > b ? 1 : a < b ? -1 : 0);
}

// ---- the prototype: getters ---------------------------------------------------------------------------------------

Result<Value> ZonedDateTimePrototype_calendarId(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "get Temporal.ZonedDateTime.prototype.calendarId"));
  return Value(newString(zoned->calendar));
}

Result<Value> ZonedDateTimePrototype_timeZoneId(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "get Temporal.ZonedDateTime.prototype.timeZoneId"));
  return Value(newString(TimeZoneIdentifier(zoned->timeZone)));
}

#define SNOWMAN_ZDT_CALENDAR_GETTER(name)                                                               \
  Result<Value> ZonedDateTimePrototype_##name(NativeCall& call) {                                       \
    ZonedDateTimeObject* zoned = TRY(thisZoned(call, "get Temporal.ZonedDateTime.prototype." #name));   \
    return CalendarDateField(zoned->calendar, isoDateTimeOf(zoned).isoDate, u"" #name);                 \
  }
SNOWMAN_ZDT_CALENDAR_GETTER(era)
SNOWMAN_ZDT_CALENDAR_GETTER(eraYear)
SNOWMAN_ZDT_CALENDAR_GETTER(year)
SNOWMAN_ZDT_CALENDAR_GETTER(month)
SNOWMAN_ZDT_CALENDAR_GETTER(monthCode)
SNOWMAN_ZDT_CALENDAR_GETTER(day)
SNOWMAN_ZDT_CALENDAR_GETTER(dayOfWeek)
SNOWMAN_ZDT_CALENDAR_GETTER(dayOfYear)
SNOWMAN_ZDT_CALENDAR_GETTER(weekOfYear)
SNOWMAN_ZDT_CALENDAR_GETTER(yearOfWeek)
SNOWMAN_ZDT_CALENDAR_GETTER(daysInWeek)
SNOWMAN_ZDT_CALENDAR_GETTER(daysInMonth)
SNOWMAN_ZDT_CALENDAR_GETTER(daysInYear)
SNOWMAN_ZDT_CALENDAR_GETTER(monthsInYear)
SNOWMAN_ZDT_CALENDAR_GETTER(inLeapYear)
#undef SNOWMAN_ZDT_CALENDAR_GETTER

#define SNOWMAN_ZDT_TIME_GETTER(name)                                                                   \
  Result<Value> ZonedDateTimePrototype_##name(NativeCall& call) {                                       \
    ZonedDateTimeObject* zoned = TRY(thisZoned(call, "get Temporal.ZonedDateTime.prototype." #name));   \
    return num(isoDateTimeOf(zoned).time.name);                                                         \
  }
SNOWMAN_ZDT_TIME_GETTER(hour)
SNOWMAN_ZDT_TIME_GETTER(minute)
SNOWMAN_ZDT_TIME_GETTER(second)
SNOWMAN_ZDT_TIME_GETTER(millisecond)
SNOWMAN_ZDT_TIME_GETTER(microsecond)
SNOWMAN_ZDT_TIME_GETTER(nanosecond)
#undef SNOWMAN_ZDT_TIME_GETTER

Result<Value> ZonedDateTimePrototype_epochMilliseconds(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "get Temporal.ZonedDateTime.prototype.epochMilliseconds"));
  EpochNanoseconds ns = zoned->epochNanoseconds;
  EpochNanoseconds ms = ns / 1'000'000 - (ns % 1'000'000 < 0 ? 1 : 0);
  return num(double(int64_t(ms)));
}

Result<Value> ZonedDateTimePrototype_epochNanoseconds(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "get Temporal.ZonedDateTime.prototype.epochNanoseconds"));
  return Value(EpochNanosecondsToBigInt(zoned->epochNanoseconds));
}

Result<Value> ZonedDateTimePrototype_hoursInDay(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "get Temporal.ZonedDateTime.prototype.hoursInDay"));
  const TimeZone& timeZone = zoned->timeZone;
  ISODate today = isoDateTimeOf(zoned).isoDate;
  ISODate tomorrow = AddDaysToISODate(today, 1);
  EpochNanoseconds todayNs = TRY(GetStartOfDay(timeZone, today));
  EpochNanoseconds tomorrowNs = TRY(GetStartOfDay(timeZone, tomorrow));
  TimeDuration diff = TimeDurationFromEpochNanosecondsDifference(tomorrowNs, todayNs);
  return num(TotalTimeDuration(diff, Unit::Hour));
}

Result<Value> ZonedDateTimePrototype_offsetNanoseconds(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "get Temporal.ZonedDateTime.prototype.offsetNanoseconds"));
  return num(double(GetOffsetNanosecondsFor(zoned->timeZone, zoned->epochNanoseconds)));
}

Result<Value> ZonedDateTimePrototype_offset(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "get Temporal.ZonedDateTime.prototype.offset"));
  return Value(newString(FormatUTCOffsetNanoseconds(GetOffsetNanosecondsFor(zoned->timeZone, zoned->epochNanoseconds))));
}

// ---- the prototype: methods ---------------------------------------------------------------------------------------

Result<Value> ZonedDateTimePrototype_with(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.with"));
  Value temporalZonedDateTimeLike = call.args[0];
  if (!TRY(IsPartialTemporalObject(temporalZonedDateTimeLike))) {
    return ThrowTypeError("Temporal.ZonedDateTime.prototype.with requires a partial date-time object");
  }
  EpochNanoseconds epochNs = zoned->epochNanoseconds;
  const TimeZone timeZone = zoned->timeZone;
  const CalendarId calendar = zoned->calendar;
  int64_t offsetNanoseconds = GetOffsetNanosecondsFor(timeZone, epochNs);
  ISODateTime isoDateTime = GetISODateTimeFor(timeZone, epochNs);
  CalendarFields fields = ISODateToFields(calendar, isoDateTime.isoDate, FieldsType::Date);
  fields.hour = isoDateTime.time.hour;
  fields.minute = isoDateTime.time.minute;
  fields.second = isoDateTime.time.second;
  fields.millisecond = isoDateTime.time.millisecond;
  fields.microsecond = isoDateTime.time.microsecond;
  fields.nanosecond = isoDateTime.time.nanosecond;
  fields.offset = FormatUTCOffsetNanoseconds(offsetNanoseconds);
  CalendarFields partialZonedDateTime = TRY(PrepareCalendarFields(calendar, temporalZonedDateTimeLike.asObject(), kFieldsDate,
                                                                  kFieldsTime | kFieldOffset, std::nullopt));
  fields = CalendarMergeFields(calendar, fields, partialZonedDateTime);
  Object* resolvedOptions = TRY(GetOptionsObject(call.args[1]));
  Disambiguation disambiguation = TRY(GetTemporalDisambiguationOption(resolvedOptions));
  OffsetOption offset = TRY(GetTemporalOffsetOption(resolvedOptions, OffsetOption::Prefer));
  Overflow overflow = TRY(GetTemporalOverflowOption(resolvedOptions));
  ISODateTime dateTimeResult = TRY(InterpretTemporalDateTimeFields(calendar, fields, overflow));
  int64_t newOffsetNanoseconds = TRY(ParseDateTimeUTCOffset(*fields.offset));
  EpochNanoseconds epochNanoseconds =
      TRY(InterpretISODateTimeOffset(dateTimeResult.isoDate, dateTimeResult.time, OffsetBehaviour::Option, newOffsetNanoseconds,
                                     timeZone, disambiguation, offset, MatchBehaviour::MatchExactly));
  return Value(TRY(CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar)));
}

Result<Value> ZonedDateTimePrototype_withPlainTime(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.withPlainTime"));
  const TimeZone timeZone = zoned->timeZone;
  const CalendarId calendar = zoned->calendar;
  ISODateTime isoDateTime = isoDateTimeOf(zoned);
  EpochNanoseconds epochNs;
  if (call.args[0].isUndefined()) {
    epochNs = TRY(GetStartOfDay(timeZone, isoDateTime.isoDate));
  } else {
    PlainTimeObject* plainTime = TRY(ToTemporalTime(call.args[0]));
    ISODateTime resultISODateTime = CombineISODateAndTimeRecord(isoDateTime.isoDate, plainTime->time);
    epochNs = TRY(GetEpochNanosecondsFor(timeZone, resultISODateTime, Disambiguation::Compatible));
  }
  return Value(TRY(CreateTemporalZonedDateTime(epochNs, timeZone, calendar)));
}

Result<Value> ZonedDateTimePrototype_withTimeZone(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.withTimeZone"));
  TimeZone timeZone = TRY(ToTemporalTimeZoneIdentifier(call.args[0]));
  return Value(TRY(CreateTemporalZonedDateTime(zoned->epochNanoseconds, timeZone, zoned->calendar)));
}

Result<Value> ZonedDateTimePrototype_withCalendar(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.withCalendar"));
  CalendarId calendar = TRY(ToTemporalCalendarIdentifier(call.args[0]));
  return Value(TRY(CreateTemporalZonedDateTime(zoned->epochNanoseconds, zoned->timeZone, calendar)));
}

Result<Value> ZonedDateTimePrototype_add(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.add"));
  return AddDurationToZonedDateTime(ArithmeticOperation::Add, zoned, call.args[0], call.args[1]);
}

Result<Value> ZonedDateTimePrototype_subtract(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.subtract"));
  return AddDurationToZonedDateTime(ArithmeticOperation::Subtract, zoned, call.args[0], call.args[1]);
}

Result<Value> ZonedDateTimePrototype_until(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.until"));
  return DifferenceTemporalZonedDateTime(DifferenceOperation::Until, zoned, call.args[0], call.args[1]);
}

Result<Value> ZonedDateTimePrototype_since(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.since"));
  return DifferenceTemporalZonedDateTime(DifferenceOperation::Since, zoned, call.args[0], call.args[1]);
}

Result<Value> ZonedDateTimePrototype_round(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.round"));
  Value roundToValue = call.args[0];
  if (roundToValue.isUndefined()) return ThrowTypeError("Temporal.ZonedDateTime.prototype.round requires options");
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
  TRY(ValidateTemporalUnitValue(smallestUnit, UnitGroup::Time, false, Unit::Day));
  if (smallestUnit == Unit::Day) {
    TRY(ValidateTemporalRoundingIncrement(roundingIncrement, 1, true));
  } else {
    TRY(ValidateTemporalRoundingIncrement(roundingIncrement, MaximumTemporalDurationRoundingIncrement(smallestUnit), false));
  }
  const EpochNanoseconds thisNs = zoned->epochNanoseconds;
  const TimeZone timeZone = zoned->timeZone;
  const CalendarId calendar = zoned->calendar;
  if (smallestUnit == Unit::Nanosecond && roundingIncrement == 1) {
    return Value(TRY(CreateTemporalZonedDateTime(thisNs, timeZone, calendar)));
  }
  ISODateTime isoDateTime = GetISODateTimeFor(timeZone, thisNs);
  EpochNanoseconds epochNanoseconds;
  if (smallestUnit == Unit::Day) {
    ISODate dateStart = isoDateTime.isoDate;
    ISODate dateEnd = AddDaysToISODate(dateStart, 1);
    EpochNanoseconds startNs = TRY(GetStartOfDay(timeZone, dateStart));
    EpochNanoseconds endNs = TRY(GetStartOfDay(timeZone, dateEnd));
    // When a transition after midnight falls back to before midnight, pieces of two calendar days interleave and
    // thisNs can lie past the start of the next day: clamp it into the day, as the reference implementation does
    // (proposal-temporal issue #3312).
    EpochNanoseconds dayNs = thisNs < endNs ? thisNs : endNs - 1;
    Int128 dayLengthNs = endNs - startNs;
    TimeDuration dayProgressNs = TimeDurationFromEpochNanosecondsDifference(dayNs, startNs);
    TimeDuration roundedDayNs = TRY(RoundTimeDurationToIncrement(dayProgressNs, dayLengthNs, roundingMode));
    epochNanoseconds = AddTimeDurationToEpochNanoseconds(roundedDayNs, startNs);
  } else {
    ISODateTime roundResult = RoundISODateTime(isoDateTime, roundingIncrement, smallestUnit, roundingMode);
    int64_t offsetNanoseconds = GetOffsetNanosecondsFor(timeZone, thisNs);
    epochNanoseconds = TRY(InterpretISODateTimeOffset(roundResult.isoDate, roundResult.time, OffsetBehaviour::Option,
                                                      offsetNanoseconds, timeZone, Disambiguation::Compatible,
                                                      OffsetOption::Prefer, MatchBehaviour::MatchExactly));
  }
  return Value(TRY(CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar)));
}

Result<Value> ZonedDateTimePrototype_equals(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.equals"));
  ZonedDateTimeObject* other = TRY(ToTemporalZonedDateTime(call.args[0]));
  if (zoned->epochNanoseconds != other->epochNanoseconds) return Value::boolean(false);
  if (!TimeZoneEquals(zoned->timeZone, other->timeZone)) return Value::boolean(false);
  return Value::boolean(zoned->calendar == other->calendar);
}

Result<Value> ZonedDateTimePrototype_toString(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.toString"));
  Object* resolvedOptions = TRY(GetOptionsObject(call.args[0]));
  ShowCalendar showCalendar = TRY(GetTemporalShowCalendarNameOption(resolvedOptions));
  int digits = TRY(GetTemporalFractionalSecondDigitsOption(resolvedOptions));
  ShowOffset showOffset = TRY(GetTemporalShowOffsetOption(resolvedOptions));
  RoundingMode roundingMode = TRY(GetRoundingModeOption(resolvedOptions, RoundingMode::Trunc));
  Unit smallestUnit = TRY(GetTemporalUnitValuedOption(resolvedOptions, u"smallestUnit"));
  ShowTimeZone showTimeZone = TRY(GetTemporalShowTimeZoneNameOption(resolvedOptions));
  TRY(ValidateTemporalUnitValue(smallestUnit, UnitGroup::Time));
  if (smallestUnit == Unit::Hour) return ThrowRangeError("smallestUnit must not be \"hour\"");
  SecondsStringPrecision precision = TRY(ToSecondsStringPrecisionRecord(smallestUnit, digits));
  return Value(newString(TemporalZonedDateTimeToString(zoned, precision.precision, showCalendar, showTimeZone, showOffset,
                                                       precision.increment, precision.unit, roundingMode)));
}

Result<Value> ZonedDateTimePrototype_toLocaleString(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.toLocaleString"));
  return Value(newString(TemporalZonedDateTimeToString(zoned, -1, ShowCalendar::Auto, ShowTimeZone::Auto, ShowOffset::Auto)));
}

Result<Value> ZonedDateTimePrototype_toJSON(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.toJSON"));
  return Value(newString(TemporalZonedDateTimeToString(zoned, -1, ShowCalendar::Auto, ShowTimeZone::Auto, ShowOffset::Auto)));
}

Result<Value> ZonedDateTimePrototype_valueOf(NativeCall&) {
  return ThrowTypeError("Temporal.ZonedDateTime can't be converted to a primitive: use compare() or epochNanoseconds");
}

Result<Value> ZonedDateTimePrototype_startOfDay(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.startOfDay"));
  EpochNanoseconds epochNanoseconds = TRY(GetStartOfDay(zoned->timeZone, isoDateTimeOf(zoned).isoDate));
  return Value(TRY(CreateTemporalZonedDateTime(epochNanoseconds, zoned->timeZone, zoned->calendar)));
}

Result<Value> ZonedDateTimePrototype_getTimeZoneTransition(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.getTimeZoneTransition"));
  const TimeZone timeZone = zoned->timeZone;
  Value directionParam = call.args[0];
  if (directionParam.isUndefined()) return ThrowTypeError("getTimeZoneTransition requires a direction");
  Object* options;
  if (directionParam.isString()) {
    options = OrdinaryObjectCreate(nullptr);
    TRY(CreateDataPropertyOrThrow(options, PropertyKey(u"direction"), directionParam));
  } else {
    options = TRY(GetOptionsObject(directionParam));
  }
  // GetDirectionOption
  std::optional<std::u16string> direction = TRY(GetStringOption(options, u"direction", {u"next", u"previous"}));
  if (!direction) return ThrowRangeError("direction is required");
  if (timeZone.isOffset()) return Value::null();
  std::optional<EpochNanoseconds> transition = *direction == u"next"
                                                   ? GetNamedTimeZoneNextTransition(timeZone, zoned->epochNanoseconds)
                                                   : GetNamedTimeZonePreviousTransition(timeZone, zoned->epochNanoseconds);
  if (!transition) return Value::null();
  return Value(TRY(CreateTemporalZonedDateTime(*transition, timeZone, zoned->calendar)));
}

Result<Value> ZonedDateTimePrototype_toInstant(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.toInstant"));
  return Value(TRY(CreateTemporalInstant(zoned->epochNanoseconds)));
}

Result<Value> ZonedDateTimePrototype_toPlainDate(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.toPlainDate"));
  return Value(TRY(CreateTemporalDate(isoDateTimeOf(zoned).isoDate, zoned->calendar)));
}

Result<Value> ZonedDateTimePrototype_toPlainTime(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.toPlainTime"));
  return Value(TRY(CreateTemporalTime(isoDateTimeOf(zoned).time)));
}

Result<Value> ZonedDateTimePrototype_toPlainDateTime(NativeCall& call) {
  ZonedDateTimeObject* zoned = TRY(thisZoned(call, "Temporal.ZonedDateTime.prototype.toPlainDateTime"));
  return Value(TRY(CreateTemporalDateTime(isoDateTimeOf(zoned), zoned->calendar)));
}

void installZonedDateTime(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor = defineConstructor(realm, Intrinsic::TemporalZonedDateTime, ZonedDateTime_construct, 2,
                                            PropertyKey(u"ZonedDateTime"), proto, Intrinsic::TemporalZonedDateTimePrototype, false);
  defineValue(TemporalNamespace(realm), PropertyKey(u"ZonedDateTime"), Value(ctor));
  defineFunction(realm, ctor, PropertyKey(u"from"), 1, ZonedDateTime_from);
  defineFunction(realm, ctor, PropertyKey(u"compare"), 2, ZonedDateTime_compare);

  struct Getter {
    const char16_t* name;
    NativeFn fn;
  };
  const Getter getters[] = {
      {u"calendarId", ZonedDateTimePrototype_calendarId},
      {u"timeZoneId", ZonedDateTimePrototype_timeZoneId},
      {u"era", ZonedDateTimePrototype_era},
      {u"eraYear", ZonedDateTimePrototype_eraYear},
      {u"year", ZonedDateTimePrototype_year},
      {u"month", ZonedDateTimePrototype_month},
      {u"monthCode", ZonedDateTimePrototype_monthCode},
      {u"day", ZonedDateTimePrototype_day},
      {u"hour", ZonedDateTimePrototype_hour},
      {u"minute", ZonedDateTimePrototype_minute},
      {u"second", ZonedDateTimePrototype_second},
      {u"millisecond", ZonedDateTimePrototype_millisecond},
      {u"microsecond", ZonedDateTimePrototype_microsecond},
      {u"nanosecond", ZonedDateTimePrototype_nanosecond},
      {u"epochMilliseconds", ZonedDateTimePrototype_epochMilliseconds},
      {u"epochNanoseconds", ZonedDateTimePrototype_epochNanoseconds},
      {u"dayOfWeek", ZonedDateTimePrototype_dayOfWeek},
      {u"dayOfYear", ZonedDateTimePrototype_dayOfYear},
      {u"weekOfYear", ZonedDateTimePrototype_weekOfYear},
      {u"yearOfWeek", ZonedDateTimePrototype_yearOfWeek},
      {u"hoursInDay", ZonedDateTimePrototype_hoursInDay},
      {u"daysInWeek", ZonedDateTimePrototype_daysInWeek},
      {u"daysInMonth", ZonedDateTimePrototype_daysInMonth},
      {u"daysInYear", ZonedDateTimePrototype_daysInYear},
      {u"monthsInYear", ZonedDateTimePrototype_monthsInYear},
      {u"inLeapYear", ZonedDateTimePrototype_inLeapYear},
      {u"offsetNanoseconds", ZonedDateTimePrototype_offsetNanoseconds},
      {u"offset", ZonedDateTimePrototype_offset},
  };
  for (const Getter& g : getters) defineAccessor(realm, proto, PropertyKey(g.name), g.fn, nullptr);

  defineFunction(realm, proto, PropertyKey(u"with"), 1, ZonedDateTimePrototype_with);
  defineFunction(realm, proto, PropertyKey(u"withPlainTime"), 0, ZonedDateTimePrototype_withPlainTime);
  defineFunction(realm, proto, PropertyKey(u"withTimeZone"), 1, ZonedDateTimePrototype_withTimeZone);
  defineFunction(realm, proto, PropertyKey(u"withCalendar"), 1, ZonedDateTimePrototype_withCalendar);
  defineFunction(realm, proto, PropertyKey(u"add"), 1, ZonedDateTimePrototype_add);
  defineFunction(realm, proto, PropertyKey(u"subtract"), 1, ZonedDateTimePrototype_subtract);
  defineFunction(realm, proto, PropertyKey(u"until"), 1, ZonedDateTimePrototype_until);
  defineFunction(realm, proto, PropertyKey(u"since"), 1, ZonedDateTimePrototype_since);
  defineFunction(realm, proto, PropertyKey(u"round"), 1, ZonedDateTimePrototype_round);
  defineFunction(realm, proto, PropertyKey(u"equals"), 1, ZonedDateTimePrototype_equals);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, ZonedDateTimePrototype_toString);
  defineFunction(realm, proto, PropertyKey(u"toLocaleString"), 0, ZonedDateTimePrototype_toLocaleString);
  defineFunction(realm, proto, PropertyKey(u"toJSON"), 0, ZonedDateTimePrototype_toJSON);
  defineFunction(realm, proto, PropertyKey(u"valueOf"), 0, ZonedDateTimePrototype_valueOf);
  defineFunction(realm, proto, PropertyKey(u"startOfDay"), 0, ZonedDateTimePrototype_startOfDay);
  defineFunction(realm, proto, PropertyKey(u"getTimeZoneTransition"), 1, ZonedDateTimePrototype_getTimeZoneTransition);
  defineFunction(realm, proto, PropertyKey(u"toInstant"), 0, ZonedDateTimePrototype_toInstant);
  defineFunction(realm, proto, PropertyKey(u"toPlainDate"), 0, ZonedDateTimePrototype_toPlainDate);
  defineFunction(realm, proto, PropertyKey(u"toPlainTime"), 0, ZonedDateTimePrototype_toPlainTime);
  defineFunction(realm, proto, PropertyKey(u"toPlainDateTime"), 0, ZonedDateTimePrototype_toPlainDateTime);
  defineValue(proto, PropertyKey(builtins::symbols().toStringTag), str(u"Temporal.ZonedDateTime"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(TemporalZonedDateTime, kOrderDefault, installZonedDateTime);

}  // namespace snowman::temporal
