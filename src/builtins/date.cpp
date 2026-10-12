// Date (21.4): time values, the constructor, Date.parse/UTC/now, and Date.prototype (with Annex B).
#include <sys/time.h>

#include <cmath>
#include <cstdio>
#include <ctime>

#include "builtins/util.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

constexpr double msPerSecond = 1000;
constexpr double msPerMinute = 60000;
constexpr double msPerHour = 3600000;
constexpr double msPerDay = 86400000;

class DateObject : public Object {
 public:
  DateObject(Object* prototype, double value) : Object(prototype), dateValue(value) {}
  bool hasDateValue() const override { return true; }
  double dateValue;  // [[DateValue]]
};

double positiveModulo(double a, double b) {
  double r = std::fmod(a, b);
  return r < 0 ? r + b : r + 0.0;
}

// 21.4.1.3 Day, 21.4.1.4 TimeWithinDay
double Day(double t) { return std::floor(t / msPerDay); }
double TimeWithinDay(double t) { return positiveModulo(t, msPerDay); }

// 21.4.1.5–21.4.1.10 years
double DaysInYear(double y) {
  if (std::fmod(y, 4) != 0) return 365;
  if (std::fmod(y, 100) != 0) return 366;
  if (std::fmod(y, 400) != 0) return 365;
  return 366;
}
double DayFromYear(double y) {
  return 365 * (y - 1970) + std::floor((y - 1969) / 4) - std::floor((y - 1901) / 100) + std::floor((y - 1601) / 400);
}
double TimeFromYear(double y) { return msPerDay * DayFromYear(y); }
double YearFromTime(double t) {
  double y = std::floor(t / (msPerDay * 365.2425)) + 1970;
  while (TimeFromYear(y) > t) y--;
  while (TimeFromYear(y + 1) <= t) y++;
  return y;
}
bool InLeapYear(double t) { return DaysInYear(YearFromTime(t)) == 366; }
double DayWithinYear(double t) { return Day(t) - DayFromYear(YearFromTime(t)); }

// The first day of each month in a common year.
const int kMonthStart[13] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};

double monthStart(int month, bool leap) { return kMonthStart[month] + (leap && month >= 2 ? 1 : 0); }

// 21.4.1.11 MonthFromTime, 21.4.1.12 DateFromTime
double MonthFromTime(double t) {
  double day = DayWithinYear(t);
  bool leap = InLeapYear(t);
  int m = 0;
  while (m < 11 && day >= monthStart(m + 1, leap)) m++;
  return m;
}
double DateFromTime(double t) {
  int m = int(MonthFromTime(t));
  return DayWithinYear(t) - monthStart(m, InLeapYear(t)) + 1;
}

// 21.4.1.13 WeekDay, 21.4.1.14–17 hours ... milliseconds
double WeekDay(double t) { return positiveModulo(Day(t) + 4, 7); }
double HourFromTime(double t) { return positiveModulo(std::floor(t / msPerHour), 24); }
double MinFromTime(double t) { return positiveModulo(std::floor(t / msPerMinute), 60); }
double SecFromTime(double t) { return positiveModulo(std::floor(t / msPerSecond), 60); }
double msFromTime(double t) { return positiveModulo(t, msPerSecond); }

double toInteger(double d) { return std::isnan(d) ? 0 : std::trunc(d) + 0.0; }

// 21.4.1.27 MakeTime
double MakeTime(double hour, double min, double sec, double ms) {
  if (!std::isfinite(hour) || !std::isfinite(min) || !std::isfinite(sec) || !std::isfinite(ms)) return NAN;
  return toInteger(hour) * msPerHour + toInteger(min) * msPerMinute + toInteger(sec) * msPerSecond + toInteger(ms);
}

// 21.4.1.28 MakeDay
double MakeDay(double year, double month, double date) {
  if (!std::isfinite(year) || !std::isfinite(month) || !std::isfinite(date)) return NAN;
  double y = toInteger(year), m = toInteger(month), dt = toInteger(date);
  double ym = y + std::floor(m / 12);
  if (!std::isfinite(ym) || std::fabs(ym) > 400000) return NAN;
  int mn = int(positiveModulo(m, 12));
  double t = TimeFromYear(ym);
  double day = Day(t) + monthStart(mn, DaysInYear(ym) == 366);
  return day + dt - 1;
}

// 21.4.1.29 MakeDate
double MakeDate(double day, double time) {
  if (!std::isfinite(day) || !std::isfinite(time)) return NAN;
  double tv = day * msPerDay + time;
  if (!std::isfinite(tv)) return NAN;
  return tv;
}

// 21.4.1.30 MakeFullYear
double MakeFullYear(double year) {
  if (std::isnan(year)) return NAN;
  double truncated = toInteger(year);
  if (truncated >= 0 && truncated <= 99) return 1900 + truncated;
  return truncated;
}

// 21.4.1.31 TimeClip
double TimeClip(double time) {
  if (!std::isfinite(time)) return NAN;
  if (std::fabs(time) > 8.64e15) return NAN;
  return toInteger(time);
}

// The host's local time zone offset (ms) at UTC time t, from the C library.
double offsetAtUTC(double t) {
  if (!std::isfinite(t)) return 0;
  // Times far outside the C library's range use the offset of a year with the same calendar layout.
  double tt = std::floor(t / 1000);
  if (tt < -67768040609740800.0 || tt > 67768036191676799.0) return 0;
  time_t seconds = time_t(tt);
  struct tm local;
  if (!localtime_r(&seconds, &local)) return 0;
  return double(local.tm_gmtoff) * 1000;
}

// 21.4.1.25 LocalTime, 21.4.1.26 UTC
double LocalTime(double t) { return t + offsetAtUTC(t); }
double UTC(double t) {
  if (!std::isfinite(t)) return NAN;
  double guess = t - offsetAtUTC(t);
  return t - offsetAtUTC(guess);
}

double currentTime() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return double(tv.tv_sec) * 1000 + double(tv.tv_usec / 1000);
}

const char* const kDayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string formatYear(double year) {
  char buf[32];
  if (year < 0) snprintf(buf, sizeof buf, "-%04d", int(-year));
  else snprintf(buf, sizeof buf, "%04d", int(year));
  return buf;
}

// 21.4.4.41.2 DateString ( tv )
std::string DateString(double tv) {
  char buf[64];
  snprintf(buf, sizeof buf, "%s %s %02d ", kDayNames[int(WeekDay(tv))], kMonthNames[int(MonthFromTime(tv))],
           int(DateFromTime(tv)));
  return buf + formatYear(YearFromTime(tv));
}

// 21.4.4.41.1 TimeString ( tv )
std::string TimeString(double tv) {
  char buf[32];
  snprintf(buf, sizeof buf, "%02d:%02d:%02d GMT", int(HourFromTime(tv)), int(MinFromTime(tv)), int(SecFromTime(tv)));
  return buf;
}

// 21.4.4.41.3 TimeZoneString ( tv ): tv is a UTC time value.
std::string TimeZoneString(double tv) {
  double offset = offsetAtUTC(tv);
  double absOffset = std::fabs(offset);
  char buf[32];
  snprintf(buf, sizeof buf, "%c%02d%02d", offset >= 0 ? '+' : '-', int(HourFromTime(absOffset)), int(MinFromTime(absOffset)));
  std::string result = buf;
  time_t seconds = time_t(std::floor(tv / 1000));
  struct tm local;
  if (std::fabs(tv) < 8.64e15 && localtime_r(&seconds, &local) && local.tm_zone && *local.tm_zone) {
    result += " (";
    result += local.tm_zone;
    result += ")";
  }
  return result;
}

// 21.4.4.41.4 ToDateString ( tv )
std::string ToDateString(double tv) {
  if (std::isnan(tv)) return "Invalid Date";
  double t = LocalTime(tv);
  return DateString(t) + " " + TimeString(t) + TimeZoneString(tv);
}

String* ascii(const std::string& s) { return newString(std::u16string(s.begin(), s.end())); }

// 21.4.1.32 Date Time String Format: YYYY[-MM[-DD]][THH:mm[:ss[.sss]][Z|±HH:mm]], years ±YYYYYY.
// Date-only forms are UTC; date-time forms without an offset are local time.
double parseISODate(std::u16string_view s) {
  size_t i = 0, n = s.size();
  auto digits = [&](size_t count, int& out) {
    if (i + count > n) return false;
    out = 0;
    for (size_t k = 0; k < count; k++) {
      char16_t c = s[i + k];
      if (c < u'0' || c > u'9') return false;
      out = out * 10 + (c - u'0');
    }
    i += count;
    return true;
  };
  int year, month = 1, day = 1, hour = 0, minute = 0, second = 0, ms = 0;
  if (i < n && (s[i] == u'+' || s[i] == u'-')) {
    bool negative = s[i] == u'-';
    i++;
    if (!digits(6, year)) return NAN;
    if (negative && year == 0) return NAN;
    if (negative) year = -year;
  } else if (!digits(4, year)) {
    return NAN;
  }
  if (i < n && s[i] == u'-') {
    i++;
    if (!digits(2, month) || month < 1 || month > 12) return NAN;
    if (i < n && s[i] == u'-') {
      i++;
      if (!digits(2, day) || day < 1) return NAN;
    }
  }
  bool hasTime = false, hasOffset = false;
  double offset = 0;
  if (i < n && s[i] == u'T') {
    hasTime = true;
    i++;
    if (!digits(2, hour) || i >= n || s[i] != u':') return NAN;
    i++;
    if (!digits(2, minute)) return NAN;
    if (i < n && s[i] == u':') {
      i++;
      if (!digits(2, second)) return NAN;
      if (i < n && s[i] == u'.') {
        i++;
        size_t start = i;
        int frac = 0, count = 0;
        while (i < n && s[i] >= u'0' && s[i] <= u'9') {
          if (count < 3) frac = frac * 10 + (s[i] - u'0');
          count++;
          i++;
        }
        if (i == start) return NAN;
        while (count < 3) frac *= 10, count++;
        ms = frac;
      }
    }
    if (hour > 24 || minute > 59 || second > 59) return NAN;
    if (hour == 24 && (minute || second || ms)) return NAN;
    if (i < n && s[i] == u'Z') {
      hasOffset = true;
      i++;
    } else if (i < n && (s[i] == u'+' || s[i] == u'-')) {
      double sign = s[i] == u'-' ? -1 : 1;
      i++;
      int oh, om;
      if (!digits(2, oh) || i >= n || s[i] != u':') return NAN;
      i++;
      if (!digits(2, om) || oh > 23 || om > 59) return NAN;
      hasOffset = true;
      offset = sign * (oh * msPerHour + om * msPerMinute);
    }
  }
  if (i != n) return NAN;
  if (day > 31) return NAN;
  double date = MakeDate(MakeDay(year, month - 1, day), MakeTime(hour, minute, second, ms));
  if (hasOffset) date -= offset;
  else if (hasTime) date = UTC(date);
  return TimeClip(date);
}

// The formats Date.prototype.toString and toUTCString produce (and their common variants):
// "Tue Feb 01 2022 10:20:30 GMT+0100 (CET)", "Tue, 01 Feb 2022 10:20:30 GMT".
double parseLegacyDate(std::u16string_view s) {
  size_t i = 0, n = s.size();
  int year = -1, month = -1, day = -1, hour = 0, minute = 0, second = 0;
  bool hasYear = false, yearNegative = false, sawTime = false, sawZone = false;
  double offset = 0;
  while (i < n) {
    char16_t c = s[i];
    if (c == u' ' || c == u',') {
      i++;
    } else if (c == u'(') {
      int depth = 0;
      while (i < n) {
        if (s[i] == u'(') depth++;
        if (s[i] == u')' && --depth == 0) {
          i++;
          break;
        }
        i++;
      }
    } else if ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z')) {
      size_t start = i;
      while (i < n && ((s[i] >= u'a' && s[i] <= u'z') || (s[i] >= u'A' && s[i] <= u'Z'))) i++;
      std::string word(s.begin() + start, s.begin() + i);
      for (char& ch : word) ch = char(std::tolower(ch));
      if (word == "gmt" || word == "utc" || word == "ut" || word == "z") {
        sawZone = true;
        continue;
      }
      bool known = false;
      for (int m = 0; m < 12; m++) {
        std::string name = kMonthNames[m];
        for (char& ch : name) ch = char(std::tolower(ch));
        if (word.size() >= 3 && word.compare(0, 3, name) == 0) {
          month = m;
          known = true;
        }
      }
      for (const char* dayName : kDayNames) {
        std::string name = dayName;
        for (char& ch : name) ch = char(std::tolower(ch));
        if (word.size() >= 3 && word.compare(0, 3, name) == 0) known = true;
      }
      if (!known) return NAN;
    } else if ((c == u'+' || c == u'-') && sawZone) {
      double sign = c == u'-' ? -1 : 1;
      i++;
      size_t start = i;
      int v = 0;
      while (i < n && s[i] >= u'0' && s[i] <= u'9') v = v * 10 + (s[i++] - u'0');
      if (i - start != 4) return NAN;
      offset = sign * ((v / 100) * msPerHour + (v % 100) * msPerMinute);
    } else if (c == u'-' && !hasYear && i + 1 < n && s[i + 1] >= u'0' && s[i + 1] <= u'9') {
      yearNegative = true;
      i++;
    } else if (c >= u'0' && c <= u'9') {
      size_t start = i;
      int v = 0;
      while (i < n && s[i] >= u'0' && s[i] <= u'9') v = v * 10 + (s[i++] - u'0');
      if (i < n && s[i] == u':') {
        hour = v;
        i++;
        minute = 0;
        while (i < n && s[i] >= u'0' && s[i] <= u'9') minute = minute * 10 + (s[i++] - u'0');
        if (i < n && s[i] == u':') {
          i++;
          second = 0;
          while (i < n && s[i] >= u'0' && s[i] <= u'9') second = second * 10 + (s[i++] - u'0');
        }
        sawTime = true;
      } else if (i - start >= 3 || yearNegative || day >= 0) {
        year = yearNegative ? -v : v;
        hasYear = true;
      } else {
        day = v;
      }
    } else {
      return NAN;
    }
  }
  if (!hasYear) return NAN;
  if (month < 0 || day < 1 || day > 31 || hour > 24 || minute > 59 || second > 59) return NAN;
  (void)sawTime;
  double date = MakeDate(MakeDay(year, month, day), MakeTime(hour, minute, second, 0));
  date = sawZone ? date - offset : UTC(date);
  return TimeClip(date);
}

double parseDate(String* string) {
  std::u16string_view s = string->view();
  double iso = parseISODate(s);
  if (!std::isnan(iso)) return iso;
  return parseLegacyDate(s);
}

// 21.4.2.1 Date ( ...values )
Result<Value> Date_construct(NativeCall& call) {
  if (!call.newTarget) return Value(ascii(ToDateString(currentTime())));
  size_t numberOfArgs = call.args.size();
  double tv;
  if (numberOfArgs == 0) {
    tv = currentTime();
  } else if (numberOfArgs == 1) {
    Value value = call.args[0];
    DateObject* d = value.isObject() ? value.asObject()->as<DateObject>() : nullptr;
    if (d) {
      tv = d->dateValue;
    } else {
      Value v = TRY(ToPrimitive(value));
      if (v.isString()) tv = parseDate(v.asString());
      else tv = TRY(ToNumber(v));
    }
    tv = TimeClip(tv);
  } else {
    double fields[7] = {0, 0, 1, 0, 0, 0, 0};
    for (size_t i = 0; i < 7 && i < numberOfArgs; i++) fields[i] = TRY(ToNumber(call.args[i]));
    double yr = MakeFullYear(fields[0]);
    double finalDate = MakeDate(MakeDay(yr, fields[1], fields[2]), MakeTime(fields[3], fields[4], fields[5], fields[6]));
    tv = TimeClip(UTC(finalDate));
  }
  return Value(TRY(OrdinaryCreateFromConstructor<DateObject>(call.newTarget, Intrinsic::DatePrototype, tv)));
}

// 21.4.3.1 Date.now ( )
Result<Value> Date_now(NativeCall&) { return num(currentTime()); }

// 21.4.3.2 Date.parse ( string )
Result<Value> Date_parse(NativeCall& call) {
  String* s = TRY(ToString(call.args[0]));
  return num(parseDate(s));
}

// 21.4.3.4 Date.UTC ( year [ , month [ , date [ , hours [ , minutes [ , seconds [ , ms ] ] ] ] ] ] )
Result<Value> Date_UTC(NativeCall& call) {
  double fields[7] = {0, 0, 1, 0, 0, 0, 0};
  fields[0] = TRY(ToNumber(call.args[0]));
  for (size_t i = 1; i < 7 && i < call.args.size(); i++) fields[i] = TRY(ToNumber(call.args[i]));
  double yr = MakeFullYear(fields[0]);
  return num(TimeClip(MakeDate(MakeDay(yr, fields[1], fields[2]), MakeTime(fields[3], fields[4], fields[5], fields[6]))));
}

Result<DateObject*> thisDate(Value v) {
  if (v.isObject()) {
    if (auto* d = v.asObject()->as<DateObject>()) return d;
  }
  return ThrowTypeError("this is not a Date object.");
}

enum class Field { FullYear, Month, Date, Day, Hours, Minutes, Seconds, Milliseconds, Year };

double fieldOf(double t, Field f) {
  switch (f) {
    case Field::FullYear: return YearFromTime(t);
    case Field::Month: return MonthFromTime(t);
    case Field::Date: return DateFromTime(t);
    case Field::Day: return WeekDay(t);
    case Field::Hours: return HourFromTime(t);
    case Field::Minutes: return MinFromTime(t);
    case Field::Seconds: return SecFromTime(t);
    case Field::Milliseconds: return msFromTime(t);
    case Field::Year: return YearFromTime(t) - 1900;
  }
  return NAN;
}

template <Field F, bool Local>
Result<Value> getter(NativeCall& call) {
  DateObject* d = TRY(thisDate(call.thisValue));
  double t = d->dateValue;
  if (std::isnan(t)) return num(NAN);
  return num(fieldOf(Local ? LocalTime(t) : t, F));
}

Result<Value> DatePrototype_getTime(NativeCall& call) {
  DateObject* d = TRY(thisDate(call.thisValue));
  return num(d->dateValue);
}

Result<Value> DatePrototype_getTimezoneOffset(NativeCall& call) {
  DateObject* d = TRY(thisDate(call.thisValue));
  double t = d->dateValue;
  if (std::isnan(t)) return num(NAN);
  return num((t - LocalTime(t)) / msPerMinute);
}

// setHours/setMinutes/setSeconds/setMilliseconds and their UTC forms: the arguments fill the time fields
// from `start` (0 hours … 3 milliseconds); absent trailing ones keep their current values.
template <int Start, bool Local>
Result<Value> timeSetter(NativeCall& call) {
  DateObject* d = TRY(thisDate(call.thisValue));
  double t = d->dateValue;
  double values[4];
  bool present[4] = {false, false, false, false};
  for (int i = Start; i < 4; i++) {
    size_t arg = size_t(i - Start);
    if (i == Start || arg < call.args.size()) {
      values[i] = TRY(ToNumber(call.args[arg]));
      present[i] = true;
    }
  }
  if (std::isnan(t)) return num(NAN);
  if (Local) t = LocalTime(t);
  double current[4] = {HourFromTime(t), MinFromTime(t), SecFromTime(t), msFromTime(t)};
  for (int i = 0; i < 4; i++) {
    if (!present[i]) values[i] = current[i];
  }
  double date = MakeDate(Day(t), MakeTime(values[0], values[1], values[2], values[3]));
  double u = TimeClip(Local ? UTC(date) : date);
  d->dateValue = u;
  return num(u);
}

// 21.4.4.20 setDate ( date ) and setUTCDate
template <bool Local>
Result<Value> setDate(NativeCall& call) {
  DateObject* d = TRY(thisDate(call.thisValue));
  double t = d->dateValue;
  double dt = TRY(ToNumber(call.args[0]));
  if (std::isnan(t)) return num(NAN);
  if (Local) t = LocalTime(t);
  double newDate = MakeDate(MakeDay(YearFromTime(t), MonthFromTime(t), dt), TimeWithinDay(t));
  double u = TimeClip(Local ? UTC(newDate) : newDate);
  d->dateValue = u;
  return num(u);
}

// 21.4.4.25 setMonth ( month [ , date ] ) and setUTCMonth
template <bool Local>
Result<Value> setMonth(NativeCall& call) {
  DateObject* d = TRY(thisDate(call.thisValue));
  double t = d->dateValue;
  double m = TRY(ToNumber(call.args[0]));
  double dt = 0;
  bool hasDate = call.args.size() > 1;
  if (hasDate) dt = TRY(ToNumber(call.args[1]));
  if (std::isnan(t)) return num(NAN);
  if (Local) t = LocalTime(t);
  if (!hasDate) dt = DateFromTime(t);
  double newDate = MakeDate(MakeDay(YearFromTime(t), m, dt), TimeWithinDay(t));
  double u = TimeClip(Local ? UTC(newDate) : newDate);
  d->dateValue = u;
  return num(u);
}

// 21.4.4.21 setFullYear ( year [ , month [ , date ] ] ) and setUTCFullYear
template <bool Local>
Result<Value> setFullYear(NativeCall& call) {
  DateObject* d = TRY(thisDate(call.thisValue));
  double t = d->dateValue;
  double y = TRY(ToNumber(call.args[0]));
  if (std::isnan(t)) t = 0;
  else if (Local) t = LocalTime(t);
  double m = call.args.size() > 1 ? TRY(ToNumber(call.args[1])) : MonthFromTime(t);
  double dt = call.args.size() > 2 ? TRY(ToNumber(call.args[2])) : DateFromTime(t);
  double newDate = MakeDate(MakeDay(y, m, dt), TimeWithinDay(t));
  double u = TimeClip(Local ? UTC(newDate) : newDate);
  d->dateValue = u;
  return num(u);
}

// 21.4.4.27 setTime ( time )
Result<Value> DatePrototype_setTime(NativeCall& call) {
  DateObject* d = TRY(thisDate(call.thisValue));
  double t = TRY(ToNumber(call.args[0]));
  d->dateValue = TimeClip(t);
  return num(d->dateValue);
}

// B.2.3.2 Date.prototype.setYear ( year )
Result<Value> DatePrototype_setYear(NativeCall& call) {
  DateObject* d = TRY(thisDate(call.thisValue));
  double t = d->dateValue;
  double y = TRY(ToNumber(call.args[0]));
  t = std::isnan(t) ? 0 : LocalTime(t);
  double yyyy = MakeFullYear(y);
  double date = MakeDate(MakeDay(yyyy, MonthFromTime(t), DateFromTime(t)), TimeWithinDay(t));
  d->dateValue = TimeClip(UTC(date));
  return num(d->dateValue);
}

enum class Format { Full, DateOnly, TimeOnly, UTCString, ISO };

template <Format F>
Result<Value> formatter(NativeCall& call) {
  DateObject* d = TRY(thisDate(call.thisValue));
  double tv = d->dateValue;
  if (std::isnan(tv)) {
    if (F == Format::ISO) return ThrowRangeError("Invalid time value");
    return str(u"Invalid Date");
  }
  switch (F) {
    case Format::Full:
      return Value(ascii(ToDateString(tv)));
    case Format::DateOnly:
      return Value(ascii(DateString(LocalTime(tv))));
    case Format::TimeOnly:
      return Value(ascii(TimeString(LocalTime(tv)) + TimeZoneString(tv)));
    case Format::UTCString: {
      char buf[64];
      snprintf(buf, sizeof buf, "%s, %02d %s ", kDayNames[int(WeekDay(tv))], int(DateFromTime(tv)),
               kMonthNames[int(MonthFromTime(tv))]);
      return Value(ascii(buf + formatYear(YearFromTime(tv)) + " " + TimeString(tv)));
    }
    case Format::ISO: {
      double year = YearFromTime(tv);
      char yearBuf[16];
      if (year >= 0 && year <= 9999) snprintf(yearBuf, sizeof yearBuf, "%04d", int(year));
      else snprintf(yearBuf, sizeof yearBuf, "%c%06d", year < 0 ? '-' : '+', int(std::fabs(year)));
      char buf[64];
      snprintf(buf, sizeof buf, "%s-%02d-%02dT%02d:%02d:%02d.%03dZ", yearBuf, int(MonthFromTime(tv)) + 1,
               int(DateFromTime(tv)), int(HourFromTime(tv)), int(MinFromTime(tv)), int(SecFromTime(tv)),
               int(msFromTime(tv)));
      return Value(ascii(buf));
    }
  }
  return Value::undefined();
}

// 21.4.4.37 Date.prototype.toJSON ( key )
Result<Value> DatePrototype_toJSON(NativeCall& call) {
  Object* o = TRY(ToObject(call.thisValue));
  Value tv = TRY(ToPrimitive(o, PreferredType::Number));
  if (tv.isNumber() && !std::isfinite(tv.asNumber())) return Value::null();
  return Invoke(o, PropertyKey(u"toISOString"));
}

Result<Value> DatePrototype_valueOf(NativeCall& call) {
  DateObject* d = TRY(thisDate(call.thisValue));
  return num(d->dateValue);
}

// 21.4.4.45 Date.prototype [ @@toPrimitive ] ( hint )
Result<Value> DatePrototype_toPrimitive(NativeCall& call) {
  Value o = call.thisValue;
  if (!o.isObject()) return ThrowTypeError("Date.prototype[Symbol.toPrimitive] called on non-object");
  Value hint = call.args[0];
  PreferredType tryFirst;
  if (hint.isString() && (hint.asString()->view() == u"string" || hint.asString()->view() == u"default"))
    tryFirst = PreferredType::String;
  else if (hint.isString() && hint.asString()->view() == u"number")
    tryFirst = PreferredType::Number;
  else
    return ThrowTypeError("Invalid hint");
  return OrdinaryToPrimitive(o.asObject(), tryFirst);
}

void installDate(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor =
      defineConstructor(realm, Intrinsic::Date, Date_construct, 7, PropertyKey(u"Date"), proto, Intrinsic::DatePrototype);
  defineFunction(realm, ctor, PropertyKey(u"now"), 0, Date_now);
  defineFunction(realm, ctor, PropertyKey(u"parse"), 1, Date_parse);
  defineFunction(realm, ctor, PropertyKey(u"UTC"), 7, Date_UTC);

  struct Fn {
    const char16_t* name;
    int length;
    NativeFn fn;
  };
  const Fn fns[] = {
      {u"getDate", 0, getter<Field::Date, true>},
      {u"getDay", 0, getter<Field::Day, true>},
      {u"getFullYear", 0, getter<Field::FullYear, true>},
      {u"getHours", 0, getter<Field::Hours, true>},
      {u"getMilliseconds", 0, getter<Field::Milliseconds, true>},
      {u"getMinutes", 0, getter<Field::Minutes, true>},
      {u"getMonth", 0, getter<Field::Month, true>},
      {u"getSeconds", 0, getter<Field::Seconds, true>},
      {u"getTime", 0, DatePrototype_getTime},
      {u"getTimezoneOffset", 0, DatePrototype_getTimezoneOffset},
      {u"getUTCDate", 0, getter<Field::Date, false>},
      {u"getUTCDay", 0, getter<Field::Day, false>},
      {u"getUTCFullYear", 0, getter<Field::FullYear, false>},
      {u"getUTCHours", 0, getter<Field::Hours, false>},
      {u"getUTCMilliseconds", 0, getter<Field::Milliseconds, false>},
      {u"getUTCMinutes", 0, getter<Field::Minutes, false>},
      {u"getUTCMonth", 0, getter<Field::Month, false>},
      {u"getUTCSeconds", 0, getter<Field::Seconds, false>},
      {u"getYear", 0, getter<Field::Year, true>},
      {u"setDate", 1, setDate<true>},
      {u"setFullYear", 3, setFullYear<true>},
      {u"setHours", 4, timeSetter<0, true>},
      {u"setMilliseconds", 1, timeSetter<3, true>},
      {u"setMinutes", 3, timeSetter<1, true>},
      {u"setMonth", 2, setMonth<true>},
      {u"setSeconds", 2, timeSetter<2, true>},
      {u"setTime", 1, DatePrototype_setTime},
      {u"setUTCDate", 1, setDate<false>},
      {u"setUTCFullYear", 3, setFullYear<false>},
      {u"setUTCHours", 4, timeSetter<0, false>},
      {u"setUTCMilliseconds", 1, timeSetter<3, false>},
      {u"setUTCMinutes", 3, timeSetter<1, false>},
      {u"setUTCMonth", 2, setMonth<false>},
      {u"setUTCSeconds", 2, timeSetter<2, false>},
      {u"setYear", 1, DatePrototype_setYear},
      {u"toDateString", 0, formatter<Format::DateOnly>},
      {u"toISOString", 0, formatter<Format::ISO>},
      {u"toJSON", 1, DatePrototype_toJSON},
      {u"toLocaleDateString", 0, formatter<Format::DateOnly>},
      {u"toLocaleString", 0, formatter<Format::Full>},
      {u"toLocaleTimeString", 0, formatter<Format::TimeOnly>},
      {u"toString", 0, formatter<Format::Full>},
      {u"toTimeString", 0, formatter<Format::TimeOnly>},
      {u"valueOf", 0, DatePrototype_valueOf},
  };
  for (const Fn& f : fns) defineFunction(realm, proto, PropertyKey(f.name), f.length, f.fn);
  BuiltinFunction* toUTCString = defineFunction(realm, proto, PropertyKey(u"toUTCString"), 0, formatter<Format::UTCString>);
  defineValue(proto, PropertyKey(u"toGMTString"), toUTCString);
  defineFunction(realm, proto, PropertyKey(symbols().toPrimitive), 1, DatePrototype_toPrimitive, Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(Date, kOrderDefault, installDate);

}  // namespace snowman::builtins
