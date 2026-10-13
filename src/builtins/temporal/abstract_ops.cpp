// Temporal abstract operations: ISO date and time math, time durations, rounding, conversions, formatting.
#include <cmath>
#include <cstdio>

#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

using builtins::toU16;

// ---------------------------------------------------------------------------------------------------------
// Units

bool IsCalendarUnit(Unit unit) { return unit == Unit::Year || unit == Unit::Month || unit == Unit::Week; }
bool IsDateUnit(Unit unit) { return unit <= Unit::Day; }
Unit LargerOfTwoTemporalUnits(Unit u1, Unit u2) { return u1 <= u2 ? u1 : u2; }

int64_t UnitLengthInNanoseconds(Unit unit) {
  switch (unit) {
    case Unit::Day: return nsPerDay;
    case Unit::Hour: return nsPerHour;
    case Unit::Minute: return nsPerMinute;
    case Unit::Second: return nsPerSecond;
    case Unit::Millisecond: return 1000000;
    case Unit::Microsecond: return 1000;
    default: return 1;
  }
}

std::u16string_view UnitSingularName(Unit unit) {
  static const char16_t* const names[] = {u"year", u"month", u"week", u"day", u"hour", u"minute", u"second",
                                          u"millisecond", u"microsecond", u"nanosecond", u"auto", u"unset"};
  return names[unsigned(unit)];
}

// ---------------------------------------------------------------------------------------------------------
// ISO dates

namespace {

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}
int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }
Int128 floorDiv128(Int128 a, Int128 b) {
  Int128 q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}
Int128 floorMod128(Int128 a, Int128 b) { return a - floorDiv128(a, b) * b; }
Int128 abs128(Int128 a) { return a < 0 ? -a : a; }

// Days from 1970-01-01 to year-month-day (proleptic Gregorian, month 1..12, day 1..31), int64 throughout.
int64_t daysFromCivil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2;
  int64_t era = floorDiv(y, 400);
  int64_t yoe = y - era * 400;
  int64_t mp = (m + 9) % 12;
  int64_t doy = (153 * mp + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

}  // namespace

bool MathematicalInLeapYear(double year) {
  if (std::fmod(year, 4) != 0) return false;
  if (std::fmod(year, 100) != 0) return true;
  return std::fmod(year, 400) == 0;
}

int32_t ISODaysInMonth(double year, int32_t month) {
  static const int32_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) return MathematicalInLeapYear(year) ? 29 : 28;
  return days[month - 1];
}

int32_t ISODaysInYear(double year) { return MathematicalInLeapYear(year) ? 366 : 365; }

int64_t ISODateToEpochDays(int64_t year, int64_t month, int64_t date) {
  int64_t resolvedYear = year + floorDiv(month, 12);
  int64_t resolvedMonth = floorMod(month, 12) + 1;
  return daysFromCivil(resolvedYear, resolvedMonth, 1) + date - 1;
}

ISODate EpochDaysToISODate(int64_t epochDays) {
  int64_t z = epochDays + 719468;
  int64_t era = floorDiv(z, 146097);
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int64_t d = doy - (153 * mp + 2) / 5 + 1;
  int64_t m = mp < 10 ? mp + 3 : mp - 9;
  return ISODate{int32_t(y + (m <= 2)), int32_t(m), int32_t(d)};
}

bool IsValidISODate(double year, double month, double day) {
  (void)year;
  if (month < 1 || month > 12) return false;
  int32_t daysInMonth = ISODaysInMonth(year, int32_t(month));
  return day >= 1 && day <= daysInMonth;
}

ISODate CreateISODateRecord(int32_t year, int32_t month, int32_t day) { return ISODate{year, month, day}; }

ISOYearMonth BalanceISOYearMonth(int64_t year, int64_t month) {
  return ISOYearMonth{year + floorDiv(month - 1, 12), int32_t(floorMod(month - 1, 12) + 1)};
}

ISODate BalanceISODate(int64_t year, int64_t month, int64_t day) {
  return EpochDaysToISODate(ISODateToEpochDays(year, month - 1, day));
}

Result<UncheckedISODate> RegulateISODate(double year, double month, double day, Overflow overflow) {
  if (overflow == Overflow::Constrain) {
    month = std::clamp(month, 1.0, 12.0);
    double daysInMonth = ISODaysInMonth(year, int32_t(month));
    day = std::clamp(day, 1.0, daysInMonth);
  } else if (!IsValidISODate(year, month, day)) {
    return ThrowRangeError("date is not valid");
  }
  return UncheckedISODate{year, int32_t(month), int32_t(day)};
}

int CompareISODate(const ISODate& a, const ISODate& b) {
  if (a.year != b.year) return a.year > b.year ? 1 : -1;
  if (a.month != b.month) return a.month > b.month ? 1 : -1;
  if (a.day != b.day) return a.day > b.day ? 1 : -1;
  return 0;
}

bool ISODateWithinLimits(double year, int32_t month, int32_t day) {
  // ISODateTimeWithinLimits(CombineISODateAndTimeRecord(isoDate, NoonTimeRecord())): -271821-04-19 through
  // 275760-09-13.
  if (year < -271821 || year > 275760) return false;
  if (year == -271821) return month > 4 || (month == 4 && day >= 19);
  if (year == 275760) return month < 9 || (month == 9 && day <= 13);
  return true;
}
bool ISODateWithinLimits(const ISODate& date) { return ISODateWithinLimits(date.year, date.month, date.day); }

bool ISOYearMonthWithinLimits(double year, int32_t month) {
  if (year < -271821 || year > 275760) return false;
  if (year == -271821) return month >= 4;
  if (year == 275760) return month <= 9;
  return true;
}

Result<ISODate> CheckISODateWithinLimits(double year, int32_t month, int32_t day) {
  if (!ISODateWithinLimits(year, month, day)) return ThrowRangeError("date is outside the supported range");
  return ISODate{int32_t(year), month, day};
}

ISODate AddDaysToISODate(const ISODate& date, int64_t days) { return BalanceISODate(date.year, date.month, int64_t(date.day) + days); }

int32_t ISODayOfWeek(const ISODate& date) {
  int64_t epochDays = ISODateToEpochDays(date.year, date.month - 1, date.day);
  // 1970-01-01 was a Thursday (4).
  return int32_t(floorMod(epochDays + 3, 7)) + 1;
}

int32_t ISODayOfYear(const ISODate& date) {
  return int32_t(ISODateToEpochDays(date.year, date.month - 1, date.day) - ISODateToEpochDays(date.year, 0, 1)) + 1;
}

YearWeek ISOWeekOfYear(const ISODate& date) {
  int64_t year = date.year;
  constexpr int wednesday = 3, thursday = 4, friday = 5, saturday = 6, daysInWeek = 7, maxWeekNumber = 53;
  int32_t dayOfYear = ISODayOfYear(date);
  int32_t dayOfWeek = ISODayOfWeek(date);
  int32_t week = (dayOfYear - dayOfWeek + 10) / daysInWeek;
  if (week < 1) {
    ISODate dayOfJan1st{date.year, 1, 1};
    int32_t dayOfWeekOfJan1st = ISODayOfWeek(dayOfJan1st);
    if (dayOfWeekOfJan1st == friday) return YearWeek{maxWeekNumber, year - 1};
    if (dayOfWeekOfJan1st == saturday && MathematicalInLeapYear(double(year - 1))) return YearWeek{maxWeekNumber, year - 1};
    return YearWeek{maxWeekNumber - 1, year - 1};
  }
  if (week == maxWeekNumber) {
    int32_t daysInYear = ISODaysInYear(double(year));
    int32_t daysLaterInYear = daysInYear - dayOfYear;
    int32_t daysAfterThursday = thursday - dayOfWeek;
    if (daysLaterInYear < daysAfterThursday) return YearWeek{1, year + 1};
  }
  (void)wednesday;
  return YearWeek{week, year};
}

// ---------------------------------------------------------------------------------------------------------
// Times

TimeRecord MidnightTimeRecord() { return TimeRecord{}; }
TimeRecord NoonTimeRecord() {
  TimeRecord t;
  t.hour = 12;
  return t;
}

bool IsValidTime(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond) {
  return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59 && millisecond >= 0 &&
         millisecond <= 999 && microsecond >= 0 && microsecond <= 999 && nanosecond >= 0 && nanosecond <= 999;
}

TimeRecord CreateTimeRecord(int32_t hour, int32_t minute, int32_t second, int32_t millisecond, int32_t microsecond,
                            int32_t nanosecond, int64_t deltaDays) {
  TimeRecord t;
  t.days = deltaDays;
  t.hour = hour;
  t.minute = minute;
  t.second = second;
  t.millisecond = millisecond;
  t.microsecond = microsecond;
  t.nanosecond = nanosecond;
  return t;
}

Result<TimeRecord> RegulateTime(double hour, double minute, double second, double millisecond, double microsecond,
                                double nanosecond, Overflow overflow) {
  if (overflow == Overflow::Constrain) {
    hour = std::clamp(hour, 0.0, 23.0);
    minute = std::clamp(minute, 0.0, 59.0);
    second = std::clamp(second, 0.0, 59.0);
    millisecond = std::clamp(millisecond, 0.0, 999.0);
    microsecond = std::clamp(microsecond, 0.0, 999.0);
    nanosecond = std::clamp(nanosecond, 0.0, 999.0);
  } else if (!IsValidTime(hour, minute, second, millisecond, microsecond, nanosecond)) {
    return ThrowRangeError("time is not valid");
  }
  return CreateTimeRecord(int32_t(hour), int32_t(minute), int32_t(second), int32_t(millisecond), int32_t(microsecond),
                          int32_t(nanosecond));
}

TimeRecord BalanceTime(Int128 hour, Int128 minute, Int128 second, Int128 millisecond, Int128 microsecond, Int128 nanosecond) {
  Int128 total = ((((hour * 60 + minute) * 60 + second) * 1000 + millisecond) * 1000 + microsecond) * 1000 + nanosecond;
  Int128 days = floorDiv128(total, nsPerDay);
  int64_t rem = int64_t(total - days * nsPerDay);
  TimeRecord t;
  t.days = int64_t(days);
  t.nanosecond = int32_t(rem % 1000);
  rem /= 1000;
  t.microsecond = int32_t(rem % 1000);
  rem /= 1000;
  t.millisecond = int32_t(rem % 1000);
  rem /= 1000;
  t.second = int32_t(rem % 60);
  rem /= 60;
  t.minute = int32_t(rem % 60);
  t.hour = int32_t(rem / 60);
  return t;
}

Int128 TimeRecordToNanoseconds(const TimeRecord& t) {
  return ((((Int128(t.hour) * 60 + t.minute) * 60 + t.second) * 1000 + t.millisecond) * 1000 + t.microsecond) * 1000 +
         t.nanosecond;
}

TimeRecord AddTime(const TimeRecord& time, TimeDuration timeDuration) {
  return BalanceTime(time.hour, time.minute, time.second, time.millisecond, time.microsecond, time.nanosecond + timeDuration);
}

TimeDuration DifferenceTime(const TimeRecord& time1, const TimeRecord& time2) {
  return TimeRecordToNanoseconds(time2) - TimeRecordToNanoseconds(time1);
}

int CompareTimeRecord(const TimeRecord& a, const TimeRecord& b) {
  Int128 x = TimeRecordToNanoseconds(a), y = TimeRecordToNanoseconds(b);
  return x > y ? 1 : x < y ? -1 : 0;
}

ISODateTime CombineISODateAndTimeRecord(const ISODate& date, const TimeRecord& time) {
  ISODateTime dt;
  dt.isoDate = date;
  dt.time = time;
  dt.time.days = 0;
  return dt;
}

int CompareISODateTime(const ISODateTime& a, const ISODateTime& b) {
  int c = CompareISODate(a.isoDate, b.isoDate);
  if (c) return c;
  return CompareTimeRecord(a.time, b.time);
}

EpochNanoseconds GetUTCEpochNanoseconds(const ISODateTime& dt) {
  int64_t days = ISODateToEpochDays(dt.isoDate.year, dt.isoDate.month - 1, dt.isoDate.day);
  return Int128(days) * nsPerDay + TimeRecordToNanoseconds(dt.time);
}

bool ISODateTimeWithinLimits(const ISODateTime& dt) {
  if (std::abs(int64_t(dt.isoDate.year)) > 300000) return false;
  EpochNanoseconds ns = GetUTCEpochNanoseconds(dt);
  return ns > nsMinInstant - nsPerDay && ns < nsMaxInstant + nsPerDay;
}

ISODateTime BalanceISODateTime(const ISODate& date, Int128 hour, Int128 minute, Int128 second, Int128 millisecond,
                               Int128 microsecond, Int128 nanosecond) {
  TimeRecord balancedTime = BalanceTime(hour, minute, second, millisecond, microsecond, nanosecond);
  ISODate balancedDate = BalanceISODate(date.year, date.month, int64_t(date.day) + balancedTime.days);
  return CombineISODateAndTimeRecord(balancedDate, balancedTime);
}

ISODateTime GetISODateTimeFromEpochNanoseconds(EpochNanoseconds ns) {
  Int128 days = floorDiv128(ns, nsPerDay);
  Int128 rem = ns - days * nsPerDay;
  ISODate date = EpochDaysToISODate(int64_t(days));
  return CombineISODateAndTimeRecord(date, BalanceTime(0, 0, 0, 0, 0, rem));
}

// ---------------------------------------------------------------------------------------------------------
// Time durations

Result<TimeDuration> CheckTimeDuration(Int128 d) {
  if (abs128(d) > kMaxTimeDuration) return ThrowRangeError("duration is out of range");
  return d;
}

TimeDuration TimeDurationFromComponents(double hours, double minutes, double seconds, double milliseconds,
                                        double microseconds, double nanoseconds) {
  Int128 min = DoubleToInt128(minutes) + DoubleToInt128(hours) * 60;
  Int128 sec = DoubleToInt128(seconds) + min * 60;
  Int128 ms = DoubleToInt128(milliseconds) + sec * 1000;
  Int128 us = DoubleToInt128(microseconds) + ms * 1000;
  return DoubleToInt128(nanoseconds) + us * 1000;
}

Result<TimeDuration> AddTimeDuration(TimeDuration one, TimeDuration two) { return CheckTimeDuration(one + two); }

Result<TimeDuration> Add24HourDaysToTimeDuration(TimeDuration d, int64_t days) {
  return CheckTimeDuration(d + Int128(days) * nsPerDay);
}

TimeDuration TimeDurationFromEpochNanosecondsDifference(EpochNanoseconds one, EpochNanoseconds two) { return one - two; }

int TimeDurationSign(TimeDuration d) { return d < 0 ? -1 : d > 0 ? 1 : 0; }
int CompareTimeDuration(TimeDuration one, TimeDuration two) { return one > two ? 1 : one < two ? -1 : 0; }

Result<TimeDuration> RoundTimeDurationToIncrement(TimeDuration d, Int128 increment, RoundingMode roundingMode) {
  return CheckTimeDuration(RoundNumberToIncrement(d, increment, roundingMode));
}

Result<TimeDuration> RoundTimeDuration(TimeDuration timeDuration, int64_t increment, Unit unit, RoundingMode roundingMode) {
  return RoundTimeDurationToIncrement(timeDuration, Int128(UnitLengthInNanoseconds(unit)) * increment, roundingMode);
}

double TotalTimeDuration(TimeDuration timeDuration, Unit unit) {
  return DivideToDouble(timeDuration, UnitLengthInNanoseconds(unit));
}

EpochNanoseconds AddTimeDurationToEpochNanoseconds(TimeDuration d, EpochNanoseconds epochNs) { return epochNs + d; }

// ---------------------------------------------------------------------------------------------------------
// Rounding

RoundingMode NegateRoundingMode(RoundingMode m) {
  switch (m) {
    case RoundingMode::Ceil: return RoundingMode::Floor;
    case RoundingMode::Floor: return RoundingMode::Ceil;
    case RoundingMode::HalfCeil: return RoundingMode::HalfFloor;
    case RoundingMode::HalfFloor: return RoundingMode::HalfCeil;
    default: return m;
  }
}

UnsignedRoundingMode GetUnsignedRoundingMode(RoundingMode m, bool negative) {
  switch (m) {
    case RoundingMode::Ceil: return negative ? UnsignedRoundingMode::Zero : UnsignedRoundingMode::Infinity;
    case RoundingMode::Floor: return negative ? UnsignedRoundingMode::Infinity : UnsignedRoundingMode::Zero;
    case RoundingMode::Expand: return UnsignedRoundingMode::Infinity;
    case RoundingMode::Trunc: return UnsignedRoundingMode::Zero;
    case RoundingMode::HalfCeil: return negative ? UnsignedRoundingMode::HalfZero : UnsignedRoundingMode::HalfInfinity;
    case RoundingMode::HalfFloor: return negative ? UnsignedRoundingMode::HalfInfinity : UnsignedRoundingMode::HalfZero;
    case RoundingMode::HalfExpand: return UnsignedRoundingMode::HalfInfinity;
    case RoundingMode::HalfTrunc: return UnsignedRoundingMode::HalfZero;
    case RoundingMode::HalfEven: return UnsignedRoundingMode::HalfEven;
  }
  return UnsignedRoundingMode::Zero;
}

bool ApplyUnsignedRoundingModeToR2(Int128 num, Int128 den, bool r1IsEven, UnsignedRoundingMode mode) {
  if (num == 0) return false;  // x = r1
  if (mode == UnsignedRoundingMode::Zero) return false;
  if (mode == UnsignedRoundingMode::Infinity) return true;
  // d1 = num/den, d2 = 1 - num/den: compare 2·num with den.
  Int128 twice = num * 2;
  if (twice < den) return false;
  if (twice > den) return true;
  if (mode == UnsignedRoundingMode::HalfZero) return false;
  if (mode == UnsignedRoundingMode::HalfInfinity) return true;
  return !r1IsEven;
}

Int128 RoundNumberToIncrement(Int128 x, Int128 increment, RoundingMode roundingMode) {
  // quotient = x / increment; r1/r2 bracket |quotient|.
  bool negative = x < 0;
  Int128 ax = negative ? -x : x;
  Int128 r1 = ax / increment;
  Int128 rem = ax % increment;
  UnsignedRoundingMode unsignedMode = GetUnsignedRoundingMode(roundingMode, negative);
  Int128 rounded = ApplyUnsignedRoundingModeToR2(rem, increment, r1 % 2 == 0, unsignedMode) ? r1 + 1 : r1;
  return (negative ? -rounded : rounded) * increment;
}

Int128 RoundNumberToIncrementAsIfPositive(Int128 x, Int128 increment, RoundingMode roundingMode) {
  Int128 r1 = floorDiv128(x, increment);
  Int128 rem = x - r1 * increment;
  UnsignedRoundingMode unsignedMode = GetUnsignedRoundingMode(roundingMode, false);
  Int128 rounded = ApplyUnsignedRoundingModeToR2(rem, increment, floorMod128(r1, 2) == 0, unsignedMode) ? r1 + 1 : r1;
  return rounded * increment;
}

namespace {

int bitLength(unsigned __int128 x) {
  int n = 0;
  while (x) {
    n++;
    x >>= 1;
  }
  return n;
}

// mantissa × 2^exponent (+ a sticky nonzero tail) to the nearest double, ties to even.
double roundToDouble(unsigned __int128 mantissa, int exponent, bool sticky) {
  if (mantissa == 0) return 0;
  int bits = bitLength(mantissa);
  if (bits > 53) {
    int shift = bits - 53;
    unsigned __int128 dropped = mantissa & ((((unsigned __int128)1) << shift) - 1);
    mantissa >>= shift;
    exponent += shift;
    unsigned __int128 half = ((unsigned __int128)1) << (shift - 1);
    bool roundUp = dropped > half || (dropped == half && (sticky || (mantissa & 1)));
    if (dropped == half && !sticky && !(mantissa & 1)) roundUp = false;
    if (roundUp) mantissa++;
  }
  return std::ldexp(double(uint64_t(mantissa)), exponent);
}

}  // namespace

double DivideToDouble(Int128 num, Int128 den) {
  if (num == 0) return 0;
  bool negative = (num < 0) != (den < 0);
  unsigned __int128 n = num < 0 ? (unsigned __int128)(-num) : (unsigned __int128)num;
  unsigned __int128 d = den < 0 ? (unsigned __int128)(-den) : (unsigned __int128)den;
  unsigned __int128 q = n / d, r = n % d;
  int exponent = 0;
  // Collect at least 66 significant bits of the quotient, then round with the remainder as sticky bit.
  while (bitLength(q) < 66) {
    r <<= 1;
    q <<= 1;
    if (r >= d) {
      r -= d;
      q |= 1;
    }
    exponent--;
  }
  double result = roundToDouble(q, exponent, r != 0);
  return negative ? -result : result;
}

double Int128ToDouble(Int128 x) {
  bool negative = x < 0;
  unsigned __int128 u = negative ? (unsigned __int128)(-x) : (unsigned __int128)x;
  double result = roundToDouble(u, 0, false);
  return negative ? -result : result;
}

// ---------------------------------------------------------------------------------------------------------
// Conversions

Result<double> ToIntegerWithTruncation(Value argument) {
  double number = TRY(ToNumber(argument));
  if (!std::isfinite(number)) return ThrowRangeError("value must be a finite number");
  return std::trunc(number) + 0.0;
}

Result<double> ToPositiveIntegerWithTruncation(Value argument) {
  double integer = TRY(ToIntegerWithTruncation(argument));
  if (integer <= 0) return ThrowRangeError("value must be a positive integer");
  return integer;
}

Result<double> ToIntegerIfIntegral(Value argument) {
  double number = TRY(ToNumber(argument));
  if (!std::isfinite(number) || std::trunc(number) != number) return ThrowRangeError("value must be an integer");
  return number + 0.0;
}

Result<std::u16string> ToMonthCode(Value argument) {
  Value monthCode = TRY(ToPrimitive(argument, PreferredType::String));
  if (!monthCode.isString()) return ThrowTypeError("monthCode must be a string");
  std::u16string_view s = monthCode.asString()->view();
  auto digit = [](char16_t c) { return c >= u'0' && c <= u'9'; };
  bool ok = (s.size() == 3 || (s.size() == 4 && s[3] == u'L')) && s[0] == u'M' && digit(s[1]) && digit(s[2]);
  if (ok && s.size() == 3 && s[1] == u'0' && s[2] == u'0') ok = false;
  if (!ok) return ThrowRangeError("invalid monthCode");
  return std::u16string(s);
}

Result<std::u16string> ToOffsetString(Value argument) {
  Value offset = TRY(ToPrimitive(argument, PreferredType::String));
  if (!offset.isString()) return ThrowTypeError("offset must be a string");
  TRY(ParseDateTimeUTCOffset(offset.asString()->view()));
  return std::u16string(offset.asString()->view());
}

// ---------------------------------------------------------------------------------------------------------
// Formatting

namespace {

std::u16string pad(int64_t value, int width) {
  char buf[32];
  snprintf(buf, sizeof buf, "%0*lld", width, (long long)value);
  return toU16(buf);
}

}  // namespace

std::u16string PadISOYear(int64_t y) {
  if (y >= 0 && y <= 9999) return pad(y, 4);
  std::u16string sign = y < 0 ? u"-" : u"+";
  return sign + pad(y < 0 ? -y : y, 6);
}

std::u16string FormatFractionalSeconds(int64_t subSecondNanoseconds, int precision) {
  if (precision == -1) {
    if (subSecondNanoseconds == 0) return u"";
    std::u16string digits = pad(subSecondNanoseconds, 9);
    while (!digits.empty() && digits.back() == u'0') digits.pop_back();
    return u"." + digits;
  }
  if (precision == 0) return u"";
  std::u16string digits = pad(subSecondNanoseconds, 9);
  return u"." + digits.substr(0, size_t(precision));
}

std::u16string FormatTimeString(int32_t hour, int32_t minute, int32_t second, int64_t subSecondNanoseconds,
                                int precision, bool separated) {
  std::u16string sep = separated ? u":" : u"";
  std::u16string hh = pad(hour, 2), mm = pad(minute, 2);
  if (precision == -2) return hh + sep + mm;
  std::u16string ss = pad(second, 2);
  return hh + sep + mm + sep + ss + FormatFractionalSeconds(subSecondNanoseconds, precision);
}

std::u16string FormatCalendarAnnotation(const CalendarId& id, ShowCalendar showCalendar) {
  if (showCalendar == ShowCalendar::Never) return u"";
  if (showCalendar == ShowCalendar::Auto && id == u"iso8601") return u"";
  std::u16string flag = showCalendar == ShowCalendar::Critical ? u"!" : u"";
  return u"[" + flag + u"u-ca=" + id + u"]";
}

std::u16string TemporalDateToString(const ISODate& d, const CalendarId& calendar, ShowCalendar showCalendar) {
  return PadISOYear(d.year) + u"-" + pad(d.month, 2) + u"-" + pad(d.day, 2) + FormatCalendarAnnotation(calendar, showCalendar);
}

std::u16string ISODateTimeToString(const ISODateTime& dt, const CalendarId& calendar, int precision, ShowCalendar showCalendar) {
  const TimeRecord& t = dt.time;
  int64_t subSecond = int64_t(t.millisecond) * 1000000 + int64_t(t.microsecond) * 1000 + t.nanosecond;
  return PadISOYear(dt.isoDate.year) + u"-" + pad(dt.isoDate.month, 2) + u"-" + pad(dt.isoDate.day, 2) + u"T" +
         FormatTimeString(t.hour, t.minute, t.second, subSecond, precision) + FormatCalendarAnnotation(calendar, showCalendar);
}

std::u16string TimeRecordToString(const TimeRecord& t, int precision) {
  int64_t subSecond = int64_t(t.millisecond) * 1000000 + int64_t(t.microsecond) * 1000 + t.nanosecond;
  return FormatTimeString(t.hour, t.minute, t.second, subSecond, precision);
}

std::u16string TemporalYearMonthToString(const ISODate& d, const CalendarId& calendar, ShowCalendar showCalendar) {
  std::u16string result = PadISOYear(d.year) + u"-" + pad(d.month, 2);
  if (showCalendar == ShowCalendar::Always || showCalendar == ShowCalendar::Critical || calendar != u"iso8601")
    result += u"-" + pad(d.day, 2);
  return result + FormatCalendarAnnotation(calendar, showCalendar);
}

std::u16string TemporalMonthDayToString(const ISODate& d, const CalendarId& calendar, ShowCalendar showCalendar) {
  std::u16string result = pad(d.month, 2) + u"-" + pad(d.day, 2);
  if (showCalendar == ShowCalendar::Always || showCalendar == ShowCalendar::Critical || calendar != u"iso8601")
    result = PadISOYear(d.year) + u"-" + result;
  return result + FormatCalendarAnnotation(calendar, showCalendar);
}

namespace {

// A non-negative integral double as decimal digits (exact, any magnitude).
std::u16string integerDigits(double v) {
  if (v < 1e21) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.0f", v);
    return toU16(buf);
  }
  // Exact: the double is an integer; print it through 128-bit (or wider) arithmetic.
  int exp;
  double m = std::frexp(v, &exp);  // v = m × 2^exp, 0.5 <= m < 1
  uint64_t mant = uint64_t(std::ldexp(m, 53));
  int shift = exp - 53;
  // Big decimal multiplication by 2^shift.
  std::vector<uint32_t> digits;  // base 1e9, little-endian
  digits.push_back(uint32_t(mant % 1000000000));
  mant /= 1000000000;
  while (mant) {
    digits.push_back(uint32_t(mant % 1000000000));
    mant /= 1000000000;
  }
  for (int i = 0; i < shift; i++) {
    uint64_t carry = 0;
    for (auto& dgt : digits) {
      uint64_t x = uint64_t(dgt) * 2 + carry;
      dgt = uint32_t(x % 1000000000);
      carry = x / 1000000000;
    }
    if (carry) digits.push_back(uint32_t(carry));
  }
  std::string out = std::to_string(digits.back());
  for (size_t i = digits.size() - 1; i-- > 0;) {
    char buf[16];
    snprintf(buf, sizeof buf, "%09u", digits[i]);
    out += buf;
  }
  return toU16(out);
}

std::u16string int128Digits(Int128 v) {
  if (v == 0) return u"0";
  std::u16string out;
  bool negative = v < 0;
  unsigned __int128 u = negative ? (unsigned __int128)(-v) : (unsigned __int128)v;
  while (u) {
    out.insert(out.begin(), char16_t(u'0' + int(u % 10)));
    u /= 10;
  }
  if (negative) out.insert(out.begin(), u'-');
  return out;
}

}  // namespace

std::u16string TemporalDurationToString(const DurationFields& duration, int precision) {
  int sign = DurationSign(duration);
  std::u16string datePart;
  auto part = [&](std::u16string& out, double value, char16_t designator) {
    if (value != 0) {
      out += integerDigits(std::fabs(value));
      out += designator;
    }
  };
  part(datePart, duration.years, u'Y');
  part(datePart, duration.months, u'M');
  part(datePart, duration.weeks, u'W');
  part(datePart, duration.days, u'D');
  std::u16string timePart;
  part(timePart, duration.hours, u'H');
  part(timePart, duration.minutes, u'M');
  // secondsDuration = TimeDurationFromComponents(0, 0, seconds, ms, µs, ns)
  TimeDuration secondsDuration = TimeDurationFromComponents(0, 0, std::fabs(duration.seconds), std::fabs(duration.milliseconds),
                                                            std::fabs(duration.microseconds), std::fabs(duration.nanoseconds));
  bool zeroMinutesAndHigher = duration.years == 0 && duration.months == 0 && duration.weeks == 0 && duration.days == 0 &&
                              duration.hours == 0 && duration.minutes == 0;
  if (secondsDuration != 0 || zeroMinutesAndHigher || precision != -1) {
    Int128 secondsPart = secondsDuration / nsPerSecond;
    int64_t subSecondsPart = int64_t(secondsDuration % nsPerSecond);
    timePart += int128Digits(secondsPart) + FormatFractionalSeconds(subSecondsPart, precision) + u"S";
  }
  std::u16string signPart = sign < 0 ? u"-" : u"";
  std::u16string result = signPart + u"P" + datePart;
  if (!timePart.empty()) result += u"T" + timePart;
  return result;
}

TimeRecord RoundTime(const TimeRecord& time, int64_t increment, Unit unit, RoundingMode roundingMode) {
  // Rounding the whole time of day in nanoseconds equals the spec's per-unit quantity: increments divide the
  // next larger unit.
  Int128 quantity = TimeRecordToNanoseconds(time);
  Int128 result = RoundNumberToIncrement(quantity, Int128(UnitLengthInNanoseconds(unit)) * increment, roundingMode);
  return BalanceTime(0, 0, 0, 0, 0, result);
}

ISODateTime RoundISODateTime(const ISODateTime& isoDateTime, int64_t increment, Unit unit, RoundingMode roundingMode) {
  TimeRecord roundedTime = RoundTime(isoDateTime.time, increment, unit, roundingMode);
  ISODate balancedDate = AddDaysToISODate(isoDateTime.isoDate, roundedTime.days);
  return CombineISODateAndTimeRecord(balancedDate, roundedTime);
}

}  // namespace snowman::temporal
