// Temporal (TC39 proposal-temporal): the shared core. Records, internal-slot classes, ISO date math, option
// getters, ISO 8601 / RFC 9557 parsing, durations and their rounding, formatting. Abstract operations are
// named and parameterised as in the spec; spec integers are int64_t/__int128 (exact) or double (Number
// values that come from JS). Files: abstract_ops.cpp (date/time math, rounding, formatting), options.cpp,
// parse.cpp, calendar.cpp (iso8601 calendar + fields), duration.cpp, plain_*.cpp, temporal.cpp (namespace).
// The time zone layer (time_zone.h, instant.cpp, zoned_date_time.cpp, now.cpp) is temporal2's.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "builtins/temporal/records.h"
#include "builtins/util.h"
#include "runtime/object.h"
#include "runtime/realm.h"
#include "runtime/result.h"

#include "builtins/temporal/time_zone.h"

namespace snowman::temporal {

using TimeDuration = __int128;  // a time duration in nanoseconds, |d| <= MaxTimeDuration
using Int128 = __int128;

// nsPerSecond … nsPerDay, nsMaxInstant/nsMinInstant and IsValidEpochNanoseconds come from time_zone.h.
// maxTimeDuration = 2^53 × 10^9 − 1
constexpr Int128 kMaxTimeDuration = Int128(9007199254740992) * nsPerSecond - 1;

// ---------------------------------------------------------------------------------------------------------
// Enumerations

// Table 21 (Temporal units), in order from largest to smallest. Auto and Unset are option sentinels.
enum class Unit : uint8_t { Year, Month, Week, Day, Hour, Minute, Second, Millisecond, Microsecond, Nanosecond, Auto, Unset };
enum class UnitGroup : uint8_t { Date, Time, DateTime };
enum class RoundingMode : uint8_t { Ceil, Floor, Expand, Trunc, HalfCeil, HalfFloor, HalfExpand, HalfTrunc, HalfEven };
enum class UnsignedRoundingMode : uint8_t { Zero, Infinity, HalfZero, HalfInfinity, HalfEven };
enum class Overflow : uint8_t { Constrain, Reject };
enum class OffsetOption : uint8_t { Prefer, Use, Ignore, Reject };
enum class ShowCalendar : uint8_t { Auto, Always, Never, Critical };
enum class ShowTimeZone : uint8_t { Auto, Never, Critical };
enum class ShowOffset : uint8_t { Auto, Never };
enum class OffsetBehaviour : uint8_t { Option, Exact, Wall };
enum class MatchBehaviour : uint8_t { MatchExactly, MatchMinutes };
enum class DifferenceOperation : uint8_t { Since, Until };
enum class ArithmeticOperation : uint8_t { Add, Subtract };

bool IsCalendarUnit(Unit unit);                  // year, month, week
bool IsDateUnit(Unit unit);                      // TemporalUnitCategory(unit) is date
Unit LargerOfTwoTemporalUnits(Unit u1, Unit u2);
int64_t UnitLengthInNanoseconds(Unit unit);      // day and smaller
std::u16string_view UnitSingularName(Unit unit);  // "year", ...

// ---------------------------------------------------------------------------------------------------------
// Calendars: identifiers are canonical lower-case strings ("iso8601" is the only one built-ins support).
using CalendarId = std::u16string;
inline const CalendarId& IsoCalendar() {
  static const CalendarId iso = u"iso8601";
  return iso;
}

// ---------------------------------------------------------------------------------------------------------
// Durations

// The ten fields of a Temporal.Duration (Number values; integral; validated by IsValidDuration).
struct DurationFields {
  double years = 0, months = 0, weeks = 0, days = 0, hours = 0, minutes = 0, seconds = 0, milliseconds = 0,
         microseconds = 0, nanoseconds = 0;
};
// Date Duration Record.
struct DateDuration {
  int64_t years = 0, months = 0, weeks = 0, days = 0;
};
// Internal Duration Record.
struct InternalDuration {
  DateDuration date;
  TimeDuration time = 0;
};

// ---------------------------------------------------------------------------------------------------------
// Internal-slot classes (one per Temporal type). Slots hold plain data only: no GC pointers to trace.

class DurationObject : public Object {
 public:
  DurationObject(Object* prototype, const DurationFields& fields) : Object(prototype), fields(fields) {}
  DurationFields fields;
};

class InstantObject : public Object {
 public:
  InstantObject(Object* prototype, EpochNanoseconds epochNanoseconds) : Object(prototype), epochNanoseconds(epochNanoseconds) {}
  EpochNanoseconds epochNanoseconds;
};

class PlainDateObject : public Object {
 public:
  PlainDateObject(Object* prototype, const ISODate& isoDate, CalendarId calendar)
      : Object(prototype), isoDate(isoDate), calendar(std::move(calendar)) {}
  ISODate isoDate;
  CalendarId calendar;
};

class PlainTimeObject : public Object {
 public:
  PlainTimeObject(Object* prototype, const TimeRecord& time) : Object(prototype), time(time) {}
  TimeRecord time;
};

class PlainDateTimeObject : public Object {
 public:
  PlainDateTimeObject(Object* prototype, const ISODateTime& isoDateTime, CalendarId calendar)
      : Object(prototype), isoDateTime(isoDateTime), calendar(std::move(calendar)) {}
  ISODateTime isoDateTime;
  CalendarId calendar;
};

class PlainYearMonthObject : public Object {
 public:
  PlainYearMonthObject(Object* prototype, const ISODate& isoDate, CalendarId calendar)
      : Object(prototype), isoDate(isoDate), calendar(std::move(calendar)) {}
  ISODate isoDate;
  CalendarId calendar;
};

class PlainMonthDayObject : public Object {
 public:
  PlainMonthDayObject(Object* prototype, const ISODate& isoDate, CalendarId calendar)
      : Object(prototype), isoDate(isoDate), calendar(std::move(calendar)) {}
  ISODate isoDate;
  CalendarId calendar;
};

class ZonedDateTimeObject : public Object {
 public:
  ZonedDateTimeObject(Object* prototype, EpochNanoseconds epochNanoseconds, const TimeZone& timeZone, CalendarId calendar)
      : Object(prototype), epochNanoseconds(epochNanoseconds), timeZone(timeZone), calendar(std::move(calendar)) {}
  EpochNanoseconds epochNanoseconds;
  TimeZone timeZone;
  CalendarId calendar;
};

// The object with the given slot class, or nullptr.
template <class T>
T* asTemporal(Value v) {
  return v.isObject() ? v.asObject()->as<T>() : nullptr;
}
// RequireInternalSlot for a method on `Name.prototype`: the receiver or a TypeError naming the method.
template <class T>
Result<T*> thisTemporal(Value thisValue, const char* method) {
  if (T* t = asTemporal<T>(thisValue)) return t;
  return ThrowTypeError(std::string(method) + " called on incompatible receiver");
}

// %Temporal%: created on first use (the class files hang their constructors on it).
Object* TemporalNamespace(Realm& realm);

// ---------------------------------------------------------------------------------------------------------
// ISO date and time math (abstract_ops.cpp). Year arguments are int64/double where they may be out of range.

bool MathematicalInLeapYear(double year);  // InLeapYear for ISO years
int32_t ISODaysInMonth(double year, int32_t month);
int32_t ISODaysInYear(double year);
// ISODateToEpochDays(year, month, date): month is 0-based and may be any integer; date may overflow.
int64_t ISODateToEpochDays(int64_t year, int64_t month, int64_t date);
// EpochDaysToISODate (the year fits int32 for |epochDays| < 7e11).
ISODate EpochDaysToISODate(int64_t epochDays);
bool IsValidISODate(double year, double month, double day);
ISODate CreateISODateRecord(int32_t year, int32_t month, int32_t day);
// BalanceISOYearMonth: callers keep |year| small enough for int32 results.
struct ISOYearMonth {
  int64_t year;
  int32_t month;
};
ISOYearMonth BalanceISOYearMonth(int64_t year, int64_t month);
ISODate BalanceISODate(int64_t year, int64_t month, int64_t day);
// RegulateISODate: the year is unchecked (callers check ISODateWithinLimits); constrain clamps month/day.
struct UncheckedISODate {
  double year;
  int32_t month, day;
};
Result<UncheckedISODate> RegulateISODate(double year, double month, double day, Overflow overflow);
int CompareISODate(const ISODate& a, const ISODate& b);
bool ISODateWithinLimits(const ISODate& date);
bool ISODateWithinLimits(double year, int32_t month, int32_t day);
bool ISOYearMonthWithinLimits(double year, int32_t month);
// Throws a RangeError unless (year, month, day) is within the ISODate limits; then the record.
Result<ISODate> CheckISODateWithinLimits(double year, int32_t month, int32_t day);
// AddDaysToISODate (BalanceISODate(y, m, d + days)).
ISODate AddDaysToISODate(const ISODate& date, int64_t days);
int32_t ISODayOfWeek(const ISODate& date);  // 1 = Monday .. 7 = Sunday
int32_t ISODayOfYear(const ISODate& date);
struct YearWeek {
  int32_t week;
  int64_t year;
};
YearWeek ISOWeekOfYear(const ISODate& date);

TimeRecord MidnightTimeRecord();
TimeRecord NoonTimeRecord();
bool IsValidTime(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond);
TimeRecord CreateTimeRecord(int32_t hour, int32_t minute, int32_t second, int32_t millisecond, int32_t microsecond,
                            int32_t nanosecond, int64_t deltaDays = 0);
Result<TimeRecord> RegulateTime(double hour, double minute, double second, double millisecond, double microsecond,
                                double nanosecond, Overflow overflow);
// BalanceTime over exact integers (any magnitude that fits 128 bits).
TimeRecord BalanceTime(Int128 hour, Int128 minute, Int128 second, Int128 millisecond, Int128 microsecond, Int128 nanosecond);
TimeRecord AddTime(const TimeRecord& time, TimeDuration timeDuration);
TimeDuration DifferenceTime(const TimeRecord& time1, const TimeRecord& time2);
int CompareTimeRecord(const TimeRecord& a, const TimeRecord& b);
Int128 TimeRecordToNanoseconds(const TimeRecord& time);  // nanoseconds since midnight (days ignored)

ISODateTime CombineISODateAndTimeRecord(const ISODate& date, const TimeRecord& time);
int CompareISODateTime(const ISODateTime& a, const ISODateTime& b);
EpochNanoseconds GetUTCEpochNanoseconds(const ISODateTime& isoDateTime);
bool ISODateTimeWithinLimits(const ISODateTime& isoDateTime);
// BalanceISODateTime(year, month, day, hour, ..., nanosecond) for a balanced date and an unbalanced time.
ISODateTime BalanceISODateTime(const ISODate& date, Int128 hour, Int128 minute, Int128 second, Int128 millisecond,
                               Int128 microsecond, Int128 nanosecond);
// The ISO date-time of an epoch-nanoseconds value plus an offset (GetISOPartsFromEpoch + offset).
ISODateTime GetISODateTimeFromEpochNanoseconds(EpochNanoseconds epochNanoseconds);

// ---------------------------------------------------------------------------------------------------------
// Time durations and rounding (abstract_ops.cpp)

// Throws a RangeError if |d| > maxTimeDuration.
Result<TimeDuration> CheckTimeDuration(Int128 d);
// TimeDurationFromComponents: exact (inputs are integral Number values).
TimeDuration TimeDurationFromComponents(double hours, double minutes, double seconds, double milliseconds,
                                        double microseconds, double nanoseconds);
Result<TimeDuration> AddTimeDuration(TimeDuration one, TimeDuration two);
Result<TimeDuration> Add24HourDaysToTimeDuration(TimeDuration d, int64_t days);
TimeDuration TimeDurationFromEpochNanosecondsDifference(EpochNanoseconds one, EpochNanoseconds two);
int TimeDurationSign(TimeDuration d);
int CompareTimeDuration(TimeDuration one, TimeDuration two);
Result<TimeDuration> RoundTimeDurationToIncrement(TimeDuration d, Int128 increment, RoundingMode roundingMode);
Result<TimeDuration> RoundTimeDuration(TimeDuration timeDuration, int64_t increment, Unit unit, RoundingMode roundingMode);
// TotalTimeDuration: d / unitLength as the nearest Number.
double TotalTimeDuration(TimeDuration timeDuration, Unit unit);
EpochNanoseconds AddTimeDurationToEpochNanoseconds(TimeDuration d, EpochNanoseconds epochNs);

RoundingMode NegateRoundingMode(RoundingMode roundingMode);
UnsignedRoundingMode GetUnsignedRoundingMode(RoundingMode roundingMode, bool negative);
// ApplyUnsignedRoundingMode for x = r1 + num/den × (r2 − r1) with 0 <= num < den (exact). Returns true for
// r2, false for r1. r1IsEven: the parity HalfEven needs ((r1 / (r2 − r1)) modulo 2 = 0).
bool ApplyUnsignedRoundingModeToR2(Int128 num, Int128 den, bool r1IsEven, UnsignedRoundingMode mode);
// RoundNumberToIncrement over exact integers (x and increment > 0).
Int128 RoundNumberToIncrement(Int128 x, Int128 increment, RoundingMode roundingMode);
Int128 RoundNumberToIncrementAsIfPositive(Int128 x, Int128 increment, RoundingMode roundingMode);
// num / den correctly rounded to a double.
double DivideToDouble(Int128 num, Int128 den);
double Int128ToDouble(Int128 x);
// An integral Number to an exact integer (|d| < 2^127).
inline Int128 DoubleToInt128(double d) { return Int128(d); }

// ---------------------------------------------------------------------------------------------------------
// Conversions (abstract_ops.cpp)

Result<double> ToIntegerWithTruncation(Value argument);
Result<double> ToPositiveIntegerWithTruncation(Value argument);
Result<double> ToIntegerIfIntegral(Value argument);
// ToMonthCode: the validated month code string (syntax only; calendar validity is checked later).
Result<std::u16string> ToMonthCode(Value argument);
// ToOffsetString: the string, after ParseDateTimeUTCOffset succeeded.
Result<std::u16string> ToOffsetString(Value argument);

// ---------------------------------------------------------------------------------------------------------
// Options (options.cpp)

Result<Object*> GetOptionsObject(Value options);
// GetOption(options, property, string, values, default): nullopt for an undefined value with no default.
Result<std::optional<std::u16string>> GetStringOption(Object* options, std::u16string_view property,
                                                       std::initializer_list<std::u16string_view> values);
Result<Overflow> GetTemporalOverflowOption(Object* options);
Result<Disambiguation> GetTemporalDisambiguationOption(Object* options);
Result<OffsetOption> GetTemporalOffsetOption(Object* options, OffsetOption fallback);
Result<ShowCalendar> GetTemporalShowCalendarNameOption(Object* options);
Result<ShowTimeZone> GetTemporalShowTimeZoneNameOption(Object* options);
Result<ShowOffset> GetTemporalShowOffsetOption(Object* options);
// Precision: -1 = auto, else 0..9 fractional digits.
Result<int> GetTemporalFractionalSecondDigitsOption(Object* options);
Result<RoundingMode> GetRoundingModeOption(Object* options, RoundingMode fallback);
Result<int64_t> GetRoundingIncrementOption(Object* options);
// ValidateTemporalRoundingIncrement(increment, dividend, inclusive)
Result<void> ValidateTemporalRoundingIncrement(int64_t increment, int64_t dividend, bool inclusive);
// GetTemporalUnitValuedOption(options, key, default): Unit::Unset when absent and default is unset. Pass
// required = true for the spec's ~required~ default (RangeError when absent).
Result<Unit> GetTemporalUnitValuedOption(Object* options, std::u16string_view key, bool required = false);
// ValidateTemporalUnitValue(value, unitGroup, extraValues): allowAuto adds « auto »; extraUnit one more.
Result<void> ValidateTemporalUnitValue(Unit value, UnitGroup unitGroup, bool allowAuto = false,
                                       std::optional<Unit> extraUnit = std::nullopt);
// MaximumTemporalDurationRoundingIncrement: 0 for unset (year..day).
int64_t MaximumTemporalDurationRoundingIncrement(Unit unit);
struct DifferenceSettings {
  Unit smallestUnit, largestUnit;
  RoundingMode roundingMode;
  int64_t roundingIncrement;
};
// GetDifferenceSettings(operation, options, unitGroup, disallowedUnits, fallbackSmallestUnit,
// smallestLargestDefaultUnit). disallowedUnits is a bit set of (1 << Unit).
Result<DifferenceSettings> GetDifferenceSettings(DifferenceOperation operation, Object* options, UnitGroup unitGroup,
                                                 uint32_t disallowedUnits, Unit fallbackSmallestUnit,
                                                 Unit smallestLargestDefaultUnit);
constexpr uint32_t unitBit(Unit u) { return 1u << unsigned(u); }
// ToSecondsStringPrecisionRecord(smallestUnit, fractionalDigitCount)
struct SecondsStringPrecision {
  int precision;  // -2 = minute, -1 = auto, 0..9
  Unit unit;
  int64_t increment;
};
Result<SecondsStringPrecision> ToSecondsStringPrecisionRecord(Unit smallestUnit, int fractionalDigitCount);

// Shared method helpers (options.cpp)
// GetOptionsObject(options) then GetTemporalOverflowOption.
Result<Overflow> GetTemporalOverflowOptionFrom(Value options);
// The roundTo/totalOf argument of round()/total(): TypeError for undefined, a String becomes
// { [key]: string } on a null-prototype object, otherwise GetOptionsObject.
Result<Object*> GetRoundingOptionsArgument(Value argument, const char16_t* key, const char* method);
// IsPartialTemporalObject(value)
Result<bool> IsPartialTemporalObject(Value value);
inline bool CalendarEquals(const CalendarId& one, const CalendarId& two) { return one == two; }

// ---------------------------------------------------------------------------------------------------------
// ISO 8601 / RFC 9557 parsing (parse.cpp). Every failure is a RangeError.

// The goal symbols ParseISODateTime accepts (a bit set).
enum ParseFormat : uint32_t {
  kDateTimeZoned = 1,    // TemporalDateTimeString[+Zoned] (= TemporalZonedDateTimeString)
  kDateTime = 2,         // TemporalDateTimeString[~Zoned]
  kInstant = 4,          // TemporalInstantString
  kTime = 8,             // TemporalTimeString
  kMonthDay = 16,        // TemporalMonthDayString
  kYearMonth = 32,       // TemporalYearMonthString
  kAllFormats = 63,
};
struct ParsedTimeZoneInfo {
  bool z = false;                                // [[Z]]
  std::optional<std::u16string> offsetString;    // [[OffsetString]]
  std::optional<std::u16string> annotation;      // [[TimeZoneAnnotation]]
};
// ISO Date-Time Parse Record. year is empty for a TemporalMonthDayString without a year; time is empty for
// ~start-of-day~ (no time part).
struct ParsedISODateTime {
  std::optional<int32_t> year;
  int32_t month = 1, day = 1;
  std::optional<TimeRecord> time;
  ParsedTimeZoneInfo timeZone;
  std::optional<std::u16string> calendar;  // the first u-ca annotation, as written
};
Result<ParsedISODateTime> ParseISODateTime(std::u16string_view isoString, uint32_t allowedFormats);
// ParseTemporalDurationString → Duration fields (unvalidated; the caller runs CreateTemporalDuration).
Result<DurationFields> ParseTemporalDurationString(std::u16string_view isoString);
// ParseDateTimeUTCOffset: offset nanoseconds of a UTCOffset[+SubMinutePrecision] string.
Result<int64_t> ParseDateTimeUTCOffset(std::u16string_view offsetString);
// Syntax checks (no throw): UTCOffset[~SubMinutePrecision] (an offset time zone identifier), UTCOffset with
// sub-minute parts, TimeZoneIANAName, AnnotationValue (a calendar name candidate).
bool IsUTCOffsetMinutePrecision(std::u16string_view s, int32_t* offsetMinutes = nullptr);
bool HasSubMinutePrecision(std::u16string_view offsetString);
bool IsTimeZoneIANANameSyntax(std::u16string_view s);
bool IsAnnotationValueSyntax(std::u16string_view s);

// ---------------------------------------------------------------------------------------------------------
// Calendars and fields (calendar.cpp)

Result<CalendarId> CanonicalizeCalendar(std::u16string_view id);
// ParseTemporalCalendarString
Result<std::u16string> ParseTemporalCalendarString(std::u16string_view string);
Result<CalendarId> ToTemporalCalendarIdentifier(Value temporalCalendarLike);
Result<CalendarId> GetTemporalCalendarIdentifierWithISODefault(Object* item);
// The calendar of a Temporal object with a [[Calendar]] slot, or nullptr.
const CalendarId* CalendarSlotOf(Value item);

// Calendar Field names (Table 19), as a bit set.
enum FieldName : uint32_t {
  kFieldEra = 1 << 0,
  kFieldEraYear = 1 << 1,
  kFieldYear = 1 << 2,
  kFieldMonth = 1 << 3,
  kFieldMonthCode = 1 << 4,
  kFieldDay = 1 << 5,
  kFieldHour = 1 << 6,
  kFieldMinute = 1 << 7,
  kFieldSecond = 1 << 8,
  kFieldMillisecond = 1 << 9,
  kFieldMicrosecond = 1 << 10,
  kFieldNanosecond = 1 << 11,
  kFieldOffset = 1 << 12,
  kFieldTimeZone = 1 << 13,
  kFieldsDate = kFieldYear | kFieldMonth | kFieldMonthCode | kFieldDay,
  kFieldsTime = kFieldHour | kFieldMinute | kFieldSecond | kFieldMillisecond | kFieldMicrosecond | kFieldNanosecond,
};
// Calendar Fields Record: every field may be unset.
struct CalendarFields {
  std::optional<std::u16string> era;
  std::optional<double> eraYear, year, month;
  std::optional<std::u16string> monthCode;
  std::optional<double> day, hour, minute, second, millisecond, microsecond, nanosecond;
  std::optional<std::u16string> offset;
  std::optional<TimeZone> timeZone;
};
// PrepareCalendarFields(calendar, fields, calendarFieldNames, nonCalendarFieldNames, requiredFieldNames).
// requiredFieldNames: a bit set, or nullopt for ~partial~.
Result<CalendarFields> PrepareCalendarFields(const CalendarId& calendar, Object* fields, uint32_t calendarFieldNames,
                                             uint32_t nonCalendarFieldNames, std::optional<uint32_t> requiredFieldNames);
CalendarFields CalendarMergeFields(const CalendarId& calendar, const CalendarFields& fields, const CalendarFields& additionalFields);
enum class FieldsType : uint8_t { Date, YearMonth, MonthDay };
Result<void> CalendarResolveFields(const CalendarId& calendar, CalendarFields& fields, FieldsType type);
Result<ISODate> CalendarDateFromFields(const CalendarId& calendar, CalendarFields& fields, Overflow overflow);
Result<ISODate> CalendarYearMonthFromFields(const CalendarId& calendar, CalendarFields& fields, Overflow overflow);
Result<ISODate> CalendarMonthDayFromFields(const CalendarId& calendar, CalendarFields& fields, Overflow overflow);
Result<ISODate> CalendarDateAdd(const CalendarId& calendar, const ISODate& isoDate, const DateDuration& duration, Overflow overflow);
DateDuration CalendarDateUntil(const CalendarId& calendar, const ISODate& one, const ISODate& two, Unit largestUnit);
// ISODateToFields(calendar, isoDate, type)
CalendarFields ISODateToFields(const CalendarId& calendar, const ISODate& isoDate, FieldsType type);
// InterpretTemporalDateTimeFields(calendar, fields, overflow)
Result<ISODateTime> InterpretTemporalDateTimeFields(const CalendarId& calendar, CalendarFields& fields, Overflow overflow);
// The month code "Mnn" of an ISO month.
std::u16string ISOMonthCode(int32_t month);

// Calendar Date Record accessors (CalendarISOToDate) for the getters every date-bearing class shares.
// `which` names the property ("year", "monthCode", "dayOfWeek", ...). Returns undefined for era/eraYear.
Value CalendarDateField(const CalendarId& calendar, const ISODate& isoDate, std::u16string_view which);

// ---------------------------------------------------------------------------------------------------------
// Formatting (abstract_ops.cpp)

std::u16string PadISOYear(int64_t year);
// FormatFractionalSeconds(subSecondNanoseconds, precision): precision -1 = auto.
std::u16string FormatFractionalSeconds(int64_t subSecondNanoseconds, int precision);
// FormatTimeString(hour, minute, second, subSecondNanoseconds, precision[, style]): precision -2 = minute.
std::u16string FormatTimeString(int32_t hour, int32_t minute, int32_t second, int64_t subSecondNanoseconds,
                                int precision, bool separated = true);
std::u16string FormatCalendarAnnotation(const CalendarId& id, ShowCalendar showCalendar);
std::u16string TemporalDateToString(const ISODate& isoDate, const CalendarId& calendar, ShowCalendar showCalendar);
std::u16string ISODateTimeToString(const ISODateTime& isoDateTime, const CalendarId& calendar, int precision,
                                   ShowCalendar showCalendar);
std::u16string TimeRecordToString(const TimeRecord& time, int precision);
std::u16string TemporalYearMonthToString(const ISODate& isoDate, const CalendarId& calendar, ShowCalendar showCalendar);
std::u16string TemporalMonthDayToString(const ISODate& isoDate, const CalendarId& calendar, ShowCalendar showCalendar);
std::u16string TemporalDurationToString(const DurationFields& duration, int precision);
// RoundISODateTime(isoDateTime, increment, unit, roundingMode) (unit day or smaller).
ISODateTime RoundISODateTime(const ISODateTime& isoDateTime, int64_t increment, Unit unit, RoundingMode roundingMode);
// RoundTime(time, increment, unit, roundingMode): the rounded time with days overflow.
TimeRecord RoundTime(const TimeRecord& time, int64_t increment, Unit unit, RoundingMode roundingMode);

// ---------------------------------------------------------------------------------------------------------
// Duration (duration.cpp)

bool IsValidDuration(const DurationFields& d);
int DurationSign(const DurationFields& d);
int DateDurationSign(const DateDuration& d);
int InternalDurationSign(const InternalDuration& d);
Unit DefaultTemporalLargestUnit(const DurationFields& d);
Result<DurationObject*> CreateTemporalDuration(const DurationFields& fields, Object* newTarget = nullptr);
DurationObject* CreateNegatedTemporalDuration(DurationObject* duration);
Result<DurationObject*> ToTemporalDuration(Value item);
Result<DateDuration> CreateDateDurationRecord(int64_t years, int64_t months, int64_t weeks, int64_t days);
// AdjustDateDurationRecord(dateDuration, days[, weeks[, months]])
Result<DateDuration> AdjustDateDurationRecord(const DateDuration& d, int64_t days, std::optional<int64_t> weeks = std::nullopt,
                                              std::optional<int64_t> months = std::nullopt);
InternalDuration CombineDateAndTimeDuration(const DateDuration& date, TimeDuration time);
InternalDuration ToInternalDurationRecord(const DurationFields& d);
InternalDuration ToInternalDurationRecordWith24HourDays(const DurationFields& d);
DateDuration ToDateDurationRecordWithoutTime(const DurationFields& d);
Result<DurationObject*> TemporalDurationFromInternal(const InternalDuration& d, Unit largestUnit, Object* newTarget = nullptr);
Result<DurationFields> TemporalDurationFieldsFromInternal(const InternalDuration& d, Unit largestUnit);

// The relative-duration machinery. timeZone == nullptr is ~unset~ (plain date-time arithmetic).
struct DurationNudgeResult {
  InternalDuration duration;
  EpochNanoseconds nudgedEpochNs;
  bool didExpandCalendarUnit;
};
Result<InternalDuration> RoundRelativeDuration(const InternalDuration& duration, EpochNanoseconds originEpochNs,
                                               EpochNanoseconds destEpochNs, const ISODateTime& isoDateTime,
                                               const TimeZone* timeZone, const CalendarId& calendar, Unit largestUnit,
                                               int64_t increment, Unit smallestUnit, RoundingMode roundingMode);
Result<double> TotalRelativeDuration(const InternalDuration& duration, EpochNanoseconds originEpochNs,
                                     EpochNanoseconds destEpochNs, const ISODateTime& isoDateTime,
                                     const TimeZone* timeZone, const CalendarId& calendar, Unit unit);
InternalDuration DifferenceISODateTime(const ISODateTime& isoDateTime1, const ISODateTime& isoDateTime2,
                                       const CalendarId& calendar, Unit largestUnit);
Result<InternalDuration> DifferencePlainDateTimeWithRounding(const ISODateTime& isoDateTime1, const ISODateTime& isoDateTime2,
                                                             const CalendarId& calendar, Unit largestUnit,
                                                             int64_t roundingIncrement, Unit smallestUnit,
                                                             RoundingMode roundingMode);
Result<double> DifferencePlainDateTimeWithTotal(const ISODateTime& isoDateTime1, const ISODateTime& isoDateTime2,
                                                const CalendarId& calendar, Unit unit);
// AddDurationToDate / AddDurationToOrSubtractDurationFromPlainDateTime etc. use this: the date part of
// AddDateTime (CalendarDateAdd after balancing the time).
Result<ISODateTime> AddDateTime(const ISODateTime& isoDateTime, const CalendarId& calendar, const InternalDuration& duration,
                                Overflow overflow);

// ---------------------------------------------------------------------------------------------------------
// Constructors and conversions of the plain types (plain_*.cpp)

Result<PlainDateObject*> CreateTemporalDate(const ISODate& isoDate, const CalendarId& calendar, Object* newTarget = nullptr);
Result<PlainTimeObject*> CreateTemporalTime(const TimeRecord& time, Object* newTarget = nullptr);
Result<PlainDateTimeObject*> CreateTemporalDateTime(const ISODateTime& isoDateTime, const CalendarId& calendar,
                                                    Object* newTarget = nullptr);
Result<PlainYearMonthObject*> CreateTemporalYearMonth(const ISODate& isoDate, const CalendarId& calendar,
                                                      Object* newTarget = nullptr);
Result<PlainMonthDayObject*> CreateTemporalMonthDay(const ISODate& isoDate, const CalendarId& calendar,
                                                    Object* newTarget = nullptr);
// ToTemporalDate(item[, options]) etc. options undefined = absent.
Result<PlainDateObject*> ToTemporalDate(Value item, Value options = Value::undefined());
Result<PlainTimeObject*> ToTemporalTime(Value item, Value options = Value::undefined());
Result<PlainDateTimeObject*> ToTemporalDateTime(Value item, Value options = Value::undefined());
Result<PlainYearMonthObject*> ToTemporalYearMonth(Value item, Value options = Value::undefined());
Result<PlainMonthDayObject*> ToTemporalMonthDay(Value item, Value options = Value::undefined());
// ToTimeRecordOrMidnight(item)
Result<TimeRecord> ToTimeRecordOrMidnight(Value item);
// ToTemporalTimeRecord(temporalTimeLike[, completeness]): partial leaves absent fields unset.
struct PartialTime {
  std::optional<double> hour, minute, second, millisecond, microsecond, nanosecond;
};
Result<PartialTime> ToTemporalTimeRecord(Object* temporalTimeLike, bool partial);

// ---------------------------------------------------------------------------------------------------------
// The time zone layer, implemented by temporal2 (zoned_date_time.cpp, instant.cpp, time_zone.cpp). zoned_stubs.cpp
// has weak fallbacks (UTC and offset zones only) so the plain types link and work without it.

Result<TimeZone> ToTemporalTimeZoneIdentifier(Value temporalTimeZoneLike);
Result<int64_t> GetOffsetNanosecondsForZone(const TimeZone& timeZone, EpochNanoseconds epochNs);
Result<EpochNanoseconds> GetEpochNanosecondsForZone(const TimeZone& timeZone, const ISODateTime& isoDateTime,
                                                    Disambiguation disambiguation);
Result<EpochNanoseconds> GetStartOfDayForZone(const TimeZone& timeZone, const ISODate& isoDate);
Result<ISODateTime> GetISODateTimeForZone(const TimeZone& timeZone, EpochNanoseconds epochNs);
// InterpretISODateTimeOffset(isoDate, time, offsetBehaviour, offsetNanoseconds, timeZone, disambiguation,
// offsetOption, matchBehaviour). time nullopt = ~start-of-day~.
Result<EpochNanoseconds> InterpretISODateTimeOffset(const ISODate& isoDate, const std::optional<TimeRecord>& time,
                                                    OffsetBehaviour offsetBehaviour, int64_t offsetNanoseconds,
                                                    const TimeZone& timeZone, Disambiguation disambiguation,
                                                    OffsetOption offsetOption, MatchBehaviour matchBehaviour);
Result<ZonedDateTimeObject*> CreateTemporalZonedDateTime(EpochNanoseconds epochNanoseconds, const TimeZone& timeZone,
                                                         const CalendarId& calendar, Object* newTarget = nullptr);
Result<InstantObject*> CreateTemporalInstant(EpochNanoseconds epochNanoseconds, Object* newTarget = nullptr);
Result<EpochNanoseconds> AddZonedDateTime(EpochNanoseconds epochNanoseconds, const TimeZone& timeZone,
                                          const CalendarId& calendar, const InternalDuration& duration, Overflow overflow);
Result<InternalDuration> DifferenceZonedDateTimeWithRounding(EpochNanoseconds ns1, EpochNanoseconds ns2,
                                                             const TimeZone& timeZone, const CalendarId& calendar,
                                                             Unit largestUnit, int64_t roundingIncrement,
                                                             Unit smallestUnit, RoundingMode roundingMode);
Result<double> DifferenceZonedDateTimeWithTotal(EpochNanoseconds ns1, EpochNanoseconds ns2, const TimeZone& timeZone,
                                                const CalendarId& calendar, Unit unit);

}  // namespace snowman::temporal
