// Weak fallbacks for the time zone layer (temporal2's: time_zone.cpp, zoned_date_time.cpp, instant.cpp),
// so the plain types link and work on their own: UTC and offset time zones only, every zone an offset zone.
// The strong definitions in temporal2's files replace these; this file goes away once both are merged.
#include "builtins/temporal/temporal.h"

#define SNOWMAN_WEAK __attribute__((weak))

namespace snowman::temporal {

namespace {

int64_t offsetNs(const TimeZone& tz) { return int64_t(tz.offsetMinutes) * nsPerMinute; }

Result<TimeZone> timeZoneFromString(std::u16string_view s) {
  std::u16string lower(s);
  for (char16_t& c : lower)
    if (c >= u'A' && c <= u'Z') c = char16_t(c + 32);
  if (lower == u"utc" || lower == u"etc/utc" || lower == u"etc/gmt" || lower == u"gmt") return TimeZone{TimeZone::kOffsetZone, 0};
  int32_t minutes;
  if (IsUTCOffsetMinutePrecision(s, &minutes)) return TimeZone{TimeZone::kOffsetZone, minutes};
  return ThrowRangeError("unknown time zone: " + utf16ToUtf8(s));
}

}  // namespace

SNOWMAN_WEAK Result<TimeZone> ToTemporalTimeZoneIdentifier(Value temporalTimeZoneLike) {
  if (auto* z = asTemporal<ZonedDateTimeObject>(temporalTimeZoneLike)) return z->timeZone;
  if (!temporalTimeZoneLike.isString()) return ThrowTypeError("time zone must be a string");
  std::u16string_view s = temporalTimeZoneLike.asString()->view();
  if (IsTimeZoneIANANameSyntax(s) || IsUTCOffsetMinutePrecision(s)) return timeZoneFromString(s);
  ParsedISODateTime parsed = TRY(ParseISODateTime(s, kAllFormats));
  if (parsed.timeZone.annotation) return timeZoneFromString(*parsed.timeZone.annotation);
  if (parsed.timeZone.z) return TimeZone{TimeZone::kOffsetZone, 0};
  if (parsed.timeZone.offsetString) {
    int32_t minutes;
    if (!IsUTCOffsetMinutePrecision(*parsed.timeZone.offsetString, &minutes))
      return ThrowRangeError("offset time zones must have minute precision");
    return TimeZone{TimeZone::kOffsetZone, minutes};
  }
  return ThrowRangeError("string has no time zone");
}

SNOWMAN_WEAK Result<int64_t> GetOffsetNanosecondsForZone(const TimeZone& timeZone, EpochNanoseconds) { return offsetNs(timeZone); }

SNOWMAN_WEAK Result<EpochNanoseconds> GetEpochNanosecondsForZone(const TimeZone& timeZone, const ISODateTime& isoDateTime,
                                                                 Disambiguation) {
  if (!ISODateTimeWithinLimits(isoDateTime)) return ThrowRangeError("date-time is outside the supported range");
  EpochNanoseconds ns = GetUTCEpochNanoseconds(isoDateTime) - offsetNs(timeZone);
  if (!IsValidEpochNanoseconds(ns)) return ThrowRangeError("date-time is outside the supported range");
  return ns;
}

SNOWMAN_WEAK Result<EpochNanoseconds> GetStartOfDayForZone(const TimeZone& timeZone, const ISODate& isoDate) {
  return GetEpochNanosecondsForZone(timeZone, CombineISODateAndTimeRecord(isoDate, MidnightTimeRecord()), Disambiguation::Compatible);
}

SNOWMAN_WEAK Result<ISODateTime> GetISODateTimeForZone(const TimeZone& timeZone, EpochNanoseconds epochNs) {
  return GetISODateTimeFromEpochNanoseconds(epochNs + offsetNs(timeZone));
}

SNOWMAN_WEAK Result<EpochNanoseconds> InterpretISODateTimeOffset(const ISODate& isoDate, const std::optional<TimeRecord>& time,
                                                                 OffsetBehaviour offsetBehaviour, int64_t offsetNanoseconds,
                                                                 const TimeZone& timeZone, Disambiguation disambiguation,
                                                                 OffsetOption offsetOption, MatchBehaviour matchBehaviour) {
  if (!time) return GetStartOfDayForZone(timeZone, isoDate);
  ISODateTime isoDateTime = CombineISODateAndTimeRecord(isoDate, *time);
  if (offsetBehaviour == OffsetBehaviour::Wall || (offsetBehaviour == OffsetBehaviour::Option && offsetOption == OffsetOption::Ignore))
    return GetEpochNanosecondsForZone(timeZone, isoDateTime, disambiguation);
  if (offsetBehaviour == OffsetBehaviour::Exact || (offsetBehaviour == OffsetBehaviour::Option && offsetOption == OffsetOption::Use)) {
    ISODateTime balanced = BalanceISODateTime(isoDate, time->hour, time->minute, time->second, time->millisecond,
                                              time->microsecond, Int128(time->nanosecond) - offsetNanoseconds);
    if (!ISODateTimeWithinLimits(balanced)) return ThrowRangeError("date-time is outside the supported range");
    EpochNanoseconds epochNs = GetUTCEpochNanoseconds(balanced);
    if (!IsValidEpochNanoseconds(epochNs)) return ThrowRangeError("date-time is outside the supported range");
    return epochNs;
  }
  // CheckISODaysRange(isoDate)
  int64_t epochDays = ISODateToEpochDays(isoDate.year, isoDate.month - 1, isoDate.day);
  if (epochDays < -100000000 || epochDays > 100000000) return ThrowRangeError("date-time is outside the supported range");
  EpochNanoseconds candidate = TRY(GetEpochNanosecondsForZone(timeZone, isoDateTime, Disambiguation::Compatible));
  int64_t candidateOffset = TRY(GetOffsetNanosecondsForZone(timeZone, candidate));
  if (candidateOffset == offsetNanoseconds) return candidate;
  if (matchBehaviour == MatchBehaviour::MatchMinutes) {
    int64_t rounded = int64_t(RoundNumberToIncrement(candidateOffset, nsPerMinute, RoundingMode::HalfExpand));
    if (rounded == offsetNanoseconds) return candidate;
  }
  if (offsetOption == OffsetOption::Reject) return ThrowRangeError("offset does not match the time zone");
  return GetEpochNanosecondsForZone(timeZone, isoDateTime, disambiguation);
}

SNOWMAN_WEAK Result<ZonedDateTimeObject*> CreateTemporalZonedDateTime(EpochNanoseconds epochNanoseconds, const TimeZone& timeZone,
                                                                      const CalendarId& calendar, Object* newTarget) {
  if (!newTarget) newTarget = builtins::intrinsic(Intrinsic::TemporalZonedDateTime);
  Object* proto = builtins::intrinsic(Intrinsic::TemporalZonedDateTimePrototype);
  if (newTarget) proto = TRY(GetPrototypeFromConstructor(newTarget, Intrinsic::TemporalZonedDateTimePrototype));
  if (!proto) proto = builtins::intrinsic(Intrinsic::ObjectPrototype);
  return make<ZonedDateTimeObject>(proto, epochNanoseconds, timeZone, calendar);
}

SNOWMAN_WEAK Result<InstantObject*> CreateTemporalInstant(EpochNanoseconds epochNanoseconds, Object* newTarget) {
  if (!newTarget) newTarget = builtins::intrinsic(Intrinsic::TemporalInstant);
  Object* proto = builtins::intrinsic(Intrinsic::TemporalInstantPrototype);
  if (newTarget) proto = TRY(GetPrototypeFromConstructor(newTarget, Intrinsic::TemporalInstantPrototype));
  if (!proto) proto = builtins::intrinsic(Intrinsic::ObjectPrototype);
  return make<InstantObject>(proto, epochNanoseconds);
}

namespace {

Result<EpochNanoseconds> AddInstant(EpochNanoseconds epochNanoseconds, TimeDuration timeDuration) {
  EpochNanoseconds result = epochNanoseconds + timeDuration;
  if (!IsValidEpochNanoseconds(result)) return ThrowRangeError("instant is outside the supported range");
  return result;
}

// DifferenceZonedDateTime(ns1, ns2, timeZone, calendar, largestUnit)
Result<InternalDuration> DifferenceZonedDateTime(EpochNanoseconds ns1, EpochNanoseconds ns2, const TimeZone& timeZone,
                                                 const CalendarId& calendar, Unit largestUnit) {
  if (ns1 == ns2) return CombineDateAndTimeDuration(DateDuration{}, 0);
  ISODateTime startDateTime = TRY(GetISODateTimeForZone(timeZone, ns1));
  ISODateTime endDateTime = TRY(GetISODateTimeForZone(timeZone, ns2));
  if (CompareISODate(startDateTime.isoDate, endDateTime.isoDate) == 0)
    return CombineDateAndTimeDuration(DateDuration{}, TimeDurationFromEpochNanosecondsDifference(ns2, ns1));
  int sign = ns2 - ns1 < 0 ? -1 : 1;
  int maxDayCorrection = sign == 1 ? 2 : 1;
  int dayCorrection = 0;
  TimeDuration timeDuration = DifferenceTime(startDateTime.time, endDateTime.time);
  if (TimeDurationSign(timeDuration) == -sign) dayCorrection++;
  bool success = false;
  ISODateTime intermediateDateTime;
  while (dayCorrection <= maxDayCorrection && !success) {
    ISODate intermediateDate = AddDaysToISODate(endDateTime.isoDate, -int64_t(dayCorrection) * sign);
    intermediateDateTime = CombineISODateAndTimeRecord(intermediateDate, startDateTime.time);
    EpochNanoseconds intermediateNs = TRY(GetEpochNanosecondsForZone(timeZone, intermediateDateTime, Disambiguation::Compatible));
    timeDuration = TimeDurationFromEpochNanosecondsDifference(ns2, intermediateNs);
    int timeSign = TimeDurationSign(timeDuration);
    if (sign != -timeSign) success = true;
    dayCorrection++;
  }
  Unit dateLargestUnit = LargerOfTwoTemporalUnits(largestUnit, Unit::Day);
  DateDuration dateDifference = CalendarDateUntil(calendar, startDateTime.isoDate, intermediateDateTime.isoDate, dateLargestUnit);
  return CombineDateAndTimeDuration(dateDifference, timeDuration);
}

}  // namespace

SNOWMAN_WEAK Result<EpochNanoseconds> AddZonedDateTime(EpochNanoseconds epochNanoseconds, const TimeZone& timeZone,
                                                       const CalendarId& calendar, const InternalDuration& duration, Overflow overflow) {
  if (DateDurationSign(duration.date) == 0) return AddInstant(epochNanoseconds, duration.time);
  ISODateTime isoDateTime = TRY(GetISODateTimeForZone(timeZone, epochNanoseconds));
  ISODate addedDate = TRY(CalendarDateAdd(calendar, isoDateTime.isoDate, duration.date, overflow));
  ISODateTime intermediateDateTime = CombineISODateAndTimeRecord(addedDate, isoDateTime.time);
  if (!ISODateTimeWithinLimits(intermediateDateTime)) return ThrowRangeError("date-time is outside the supported range");
  EpochNanoseconds intermediateNs = TRY(GetEpochNanosecondsForZone(timeZone, intermediateDateTime, Disambiguation::Compatible));
  return AddInstant(intermediateNs, duration.time);
}

SNOWMAN_WEAK Result<InternalDuration> DifferenceZonedDateTimeWithRounding(EpochNanoseconds ns1, EpochNanoseconds ns2,
                                                                          const TimeZone& timeZone, const CalendarId& calendar,
                                                                          Unit largestUnit, int64_t roundingIncrement,
                                                                          Unit smallestUnit, RoundingMode roundingMode) {
  if (!IsDateUnit(largestUnit)) {
    TimeDuration timeDuration = TimeDurationFromEpochNanosecondsDifference(ns2, ns1);
    timeDuration = TRY(RoundTimeDuration(timeDuration, roundingIncrement, smallestUnit, roundingMode));
    return CombineDateAndTimeDuration(DateDuration{}, timeDuration);
  }
  InternalDuration difference = TRY(DifferenceZonedDateTime(ns1, ns2, timeZone, calendar, largestUnit));
  if (smallestUnit == Unit::Nanosecond && roundingIncrement == 1) return difference;
  ISODateTime dateTime = TRY(GetISODateTimeForZone(timeZone, ns1));
  return RoundRelativeDuration(difference, ns1, ns2, dateTime, &timeZone, calendar, largestUnit, roundingIncrement,
                               smallestUnit, roundingMode);
}

SNOWMAN_WEAK Result<double> DifferenceZonedDateTimeWithTotal(EpochNanoseconds ns1, EpochNanoseconds ns2, const TimeZone& timeZone,
                                                             const CalendarId& calendar, Unit unit) {
  if (!IsDateUnit(unit)) return TotalTimeDuration(TimeDurationFromEpochNanosecondsDifference(ns2, ns1), unit);
  InternalDuration difference = TRY(DifferenceZonedDateTime(ns1, ns2, timeZone, calendar, unit));
  ISODateTime dateTime = TRY(GetISODateTimeForZone(timeZone, ns1));
  return TotalRelativeDuration(difference, ns1, ns2, dateTime, &timeZone, calendar, unit);
}

}  // namespace snowman::temporal
