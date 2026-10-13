// Temporal.ZonedDateTime (proposal-temporal §6) and the time zone operations temporal.h declares for the core.
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

}  // namespace snowman::temporal
