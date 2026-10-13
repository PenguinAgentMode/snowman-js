// ISO 8601 / RFC 9557 string parsing for Temporal (the grammar of proposal-temporal §13.33 "ISO 8601
// grammar"): date-times with offsets and bracketed annotations, times, year-months, month-days, durations,
// UTC offsets. Every failure is a RangeError.
#include <cmath>

#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

namespace {

bool isDigit(char16_t c) { return c >= u'0' && c <= u'9'; }
bool isAlpha(char16_t c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); }
bool isSign(char16_t c) { return c == u'+' || c == u'-'; }

struct TimeParts {
  int32_t hour = 0, minute = 0, second = 0;
  int64_t fraction = 0;  // nanoseconds
};

struct Annotation {
  bool critical;
  std::u16string key, value;
};

// A recursive-descent parser with backtracking: each parseX either consumes its production and returns
// true, or leaves pos where it was and returns false.
class Parser {
 public:
  explicit Parser(std::u16string_view s) : s_(s) {}

  size_t pos = 0;
  bool atEnd() const { return pos == s_.size(); }
  char16_t peek(size_t ahead = 0) const { return pos + ahead < s_.size() ? s_[pos + ahead] : 0; }
  bool eat(char16_t c) {
    if (peek() == c) {
      pos++;
      return true;
    }
    return false;
  }
  std::u16string_view text(size_t from, size_t to) const { return s_.substr(from, to - from); }

  bool digits(int count, int32_t& out) {
    if (pos + size_t(count) > s_.size()) return false;
    int32_t v = 0;
    for (int i = 0; i < count; i++) {
      char16_t c = s_[pos + size_t(i)];
      if (!isDigit(c)) return false;
      v = v * 10 + (c - u'0');
    }
    out = v;
    pos += size_t(count);
    return true;
  }

  // DateYear ::: DecimalDigit{4} | ASCIISign DecimalDigit{6} (not -000000)
  bool dateYear(int32_t& year) {
    size_t start = pos;
    if (isSign(peek())) {
      bool negative = peek() == u'-';
      pos++;
      int32_t v;
      if (!digits(6, v) || (negative && v == 0)) return pos = start, false;
      year = negative ? -v : v;
      return true;
    }
    return digits(4, year);
  }
  bool twoDigitsInRange(int32_t lo, int32_t hi, int32_t& out) {
    size_t start = pos;
    int32_t v;
    if (!digits(2, v) || v < lo || v > hi) return pos = start, false;
    out = v;
    return true;
  }
  bool dateMonth(int32_t& m) { return twoDigitsInRange(1, 12, m); }
  bool dateDay(int32_t& d) { return twoDigitsInRange(1, 31, d); }

  // Date ::: DateYear - DateMonth - DateDay | DateYear DateMonth DateDay
  bool date(int32_t& y, int32_t& m, int32_t& d) {
    size_t start = pos;
    if (!dateYear(y)) return false;
    if (eat(u'-')) {
      if (dateMonth(m) && eat(u'-') && dateDay(d)) return true;
    } else if (dateMonth(m) && dateDay(d)) {
      return true;
    }
    pos = start;
    return false;
  }

  // DateSpecYearMonth ::: DateYear -opt DateMonth
  bool dateSpecYearMonth(int32_t& y, int32_t& m) {
    size_t start = pos;
    if (!dateYear(y)) return false;
    eat(u'-');
    if (dateMonth(m)) return true;
    pos = start;
    return false;
  }

  // DateSpecMonthDay ::: --opt DateMonth -opt DateDay
  bool dateSpecMonthDay(int32_t& m, int32_t& d) {
    size_t start = pos;
    if (peek() == u'-' && peek(1) == u'-') pos += 2;
    if (!dateMonth(m)) return pos = start, false;
    eat(u'-');
    if (dateDay(d)) return true;
    pos = start;
    return false;
  }

  // TemporalDecimalFraction ::: TemporalDecimalSeparator DecimalDigit{1,9}
  bool fraction(int64_t& ns) {
    size_t start = pos;
    if (!(peek() == u'.' || peek() == u',')) return false;
    pos++;
    int count = 0;
    int64_t v = 0;
    while (isDigit(peek()) && count < 9) {
      v = v * 10 + (peek() - u'0');
      pos++;
      count++;
    }
    if (count == 0 || isDigit(peek())) return pos = start, false;
    for (int i = count; i < 9; i++) v *= 10;
    ns = v;
    return true;
  }

  // Time ::: Hour | Hour : MinuteSecond | Hour MinuteSecond | Hour : MinuteSecond : TimeSecond Fraction? |
  //          Hour MinuteSecond TimeSecond Fraction?
  bool time(TimeParts& t) {
    size_t start = pos;
    if (!twoDigitsInRange(0, 23, t.hour)) return false;
    t.minute = t.second = 0;
    t.fraction = 0;
    if (eat(u':')) {
      if (!twoDigitsInRange(0, 59, t.minute)) return pos = start, false;
      if (eat(u':')) {
        if (!twoDigitsInRange(0, 60, t.second)) return pos = start, false;
        fraction(t.fraction);
      }
      return true;
    }
    if (twoDigitsInRange(0, 59, t.minute)) {
      if (twoDigitsInRange(0, 60, t.second)) fraction(t.fraction);
    }
    return true;
  }

  // UTCOffset[SubMinutePrecision] ::: ASCIISign Hour (TimeSeparator MinuteSecond (TimeSeparator MinuteSecond
  // Fraction?)?)?, separators all present or all absent.
  bool utcOffset(bool subMinute, bool* hadSubMinute = nullptr) {
    size_t start = pos;
    if (!isSign(peek())) return false;
    pos++;
    int32_t h, m, sec;
    if (!twoDigitsInRange(0, 23, h)) return pos = start, false;
    if (hadSubMinute) *hadSubMinute = false;
    if (eat(u':')) {
      if (!twoDigitsInRange(0, 59, m)) return pos = start, false;
      if (subMinute && peek() == u':') {
        size_t save = pos;
        pos++;
        if (twoDigitsInRange(0, 59, sec)) {
          int64_t f;
          fraction(f);
          if (hadSubMinute) *hadSubMinute = true;
        } else {
          pos = save;
        }
      }
      return true;
    }
    if (twoDigitsInRange(0, 59, m)) {
      if (subMinute && twoDigitsInRange(0, 59, sec)) {
        int64_t f;
        fraction(f);
        if (hadSubMinute) *hadSubMinute = true;
      }
    }
    return true;
  }

  // DateTimeUTCOffset[Z] ::: [+Z] UTCDesignator | UTCOffset[+SubMinutePrecision]
  bool dateTimeUTCOffset(bool allowZ, ParsedTimeZoneInfo& tz) {
    if (peek() == u'Z' || peek() == u'z') {
      if (!allowZ) return false;
      pos++;
      tz.z = true;
      return true;
    }
    size_t start = pos;
    if (!utcOffset(true)) return false;
    tz.offsetString = std::u16string(text(start, pos));
    return true;
  }

  // TimeZoneIANANameComponent ::: TZLeadingChar TZChar* (but not . or ..)
  bool tzNameComponent() {
    size_t start = pos;
    char16_t c = peek();
    if (!(isAlpha(c) || c == u'.' || c == u'_')) return false;
    pos++;
    while (true) {
      c = peek();
      if (isAlpha(c) || c == u'.' || c == u'_' || isDigit(c) || c == u'-' || c == u'+') pos++;
      else break;
    }
    std::u16string_view comp = text(start, pos);
    if (comp == u"." || comp == u"..") return pos = start, false;
    return true;
  }
  bool tzIANAName() {
    size_t start = pos;
    if (!tzNameComponent()) return false;
    while (peek() == u'/') {
      pos++;
      if (!tzNameComponent()) return pos = start, false;
    }
    return true;
  }

  // TimeZoneAnnotation ::: [ AnnotationCriticalFlag? TimeZoneIdentifier ]
  bool timeZoneAnnotation(ParsedTimeZoneInfo& tz) {
    size_t start = pos;
    if (!eat(u'[')) return false;
    eat(u'!');
    size_t idStart = pos;
    if (!utcOffset(false) && !tzIANAName()) return pos = start, false;
    size_t idEnd = pos;
    if (!eat(u']')) return pos = start, false;
    tz.annotation = std::u16string(text(idStart, idEnd));
    return true;
  }

  // Annotation ::: [ AnnotationCriticalFlag? AnnotationKey = AnnotationValue ]
  bool annotation(Annotation& a) {
    size_t start = pos;
    if (!eat(u'[')) return false;
    a.critical = eat(u'!');
    size_t keyStart = pos;
    char16_t c = peek();
    if (!((c >= u'a' && c <= u'z') || c == u'_')) return pos = start, false;
    pos++;
    while (true) {
      c = peek();
      if ((c >= u'a' && c <= u'z') || c == u'_' || isDigit(c) || c == u'-') pos++;
      else break;
    }
    a.key = std::u16string(text(keyStart, pos));
    if (!eat(u'=')) return pos = start, false;
    size_t valueStart = pos;
    if (!annotationValue()) return pos = start, false;
    a.value = std::u16string(text(valueStart, pos));
    if (!eat(u']')) return pos = start, false;
    return true;
  }
  // AnnotationValue ::: AnnotationValueComponent (- AnnotationValueComponent)*
  bool annotationValue() {
    size_t start = pos;
    auto component = [&] {
      size_t s = pos;
      while (isAlpha(peek()) || isDigit(peek())) pos++;
      return pos > s;
    };
    if (!component()) return false;
    while (peek() == u'-') {
      pos++;
      if (!component()) return pos = start, false;
    }
    return true;
  }

  // TimeZoneAnnotation? Annotations? then the end of input.
  bool annotationsToEnd(ParsedTimeZoneInfo& tz, std::vector<Annotation>& annotations, bool requireTimeZone) {
    size_t start = pos;
    bool hasTz = timeZoneAnnotation(tz);
    if (requireTimeZone && !hasTz) return false;
    Annotation a;
    while (annotation(a)) annotations.push_back(a);
    if (!atEnd()) {
      pos = start;
      tz.annotation.reset();
      annotations.clear();
      return false;
    }
    return true;
  }

 private:
  std::u16string_view s_;
};

struct RawParse {
  std::optional<int32_t> year;
  int32_t month = 1, day = 1;
  std::optional<TimeParts> time;
  ParsedTimeZoneInfo tz;
  std::vector<Annotation> annotations;
};

// DateTime[Z, TimeRequired] ::: [~TimeRequired] Date | Date DateTimeSeparator Time DateTimeUTCOffset[?Z]?
bool parseDateTime(Parser& p, bool allowZ, bool timeRequired, RawParse& r) {
  size_t start = p.pos;
  int32_t y, m, d;
  if (!p.date(y, m, d)) return false;
  r.year = y;
  r.month = m;
  r.day = d;
  char16_t sep = p.peek();
  if (sep == u'T' || sep == u't' || sep == u' ') {
    size_t save = p.pos;
    p.pos++;
    TimeParts t;
    if (p.time(t)) {
      r.time = t;
      ParsedTimeZoneInfo tz;
      if (p.dateTimeUTCOffset(allowZ, tz)) r.tz = tz;
      return true;
    }
    p.pos = save;
  }
  if (timeRequired) {
    p.pos = start;
    return false;
  }
  return true;
}

// AnnotatedDateTime[Zoned, TimeRequired]
bool parseAnnotatedDateTime(std::u16string_view s, bool zoned, bool timeRequired, RawParse& r) {
  Parser p(s);
  if (!parseDateTime(p, zoned, timeRequired, r)) return false;
  return p.annotationsToEnd(r.tz, r.annotations, zoned);
}

// TemporalInstantString ::: Date DateTimeSeparator Time DateTimeUTCOffset[+Z] TimeZoneAnnotation? Annotations?
bool parseInstant(std::u16string_view s, RawParse& r) {
  Parser p(s);
  if (!parseDateTime(p, true, true, r)) return false;
  if (!r.tz.z && !r.tz.offsetString) return false;
  return p.annotationsToEnd(r.tz, r.annotations, false);
}

// AnnotatedTime ::: TimeDesignator Time DateTimeUTCOffset[~Z]? Annotations… | Time DateTimeUTCOffset[~Z]? …
// (without the designator, it may not also be a DateSpecMonthDay or DateSpecYearMonth).
bool parseAnnotatedTime(std::u16string_view s, RawParse& r) {
  Parser p(s);
  bool designator = p.eat(u'T') || p.eat(u't');
  TimeParts t;
  if (!p.time(t)) return false;
  r.time = t;
  p.dateTimeUTCOffset(false, r.tz);
  size_t timeEnd = p.pos;
  if (!designator) {
    std::u16string_view timeText = s.substr(0, timeEnd);
    int32_t a, b;
    Parser md(timeText);
    if (md.dateSpecMonthDay(a, b) && md.atEnd() && b <= ISODaysInMonth(1972, a)) return false;
    Parser ym(timeText);
    if (ym.dateSpecYearMonth(a, b) && ym.atEnd()) return false;
  }
  return p.annotationsToEnd(r.tz, r.annotations, false);
}

// AnnotatedYearMonth ::: DateSpecYearMonth TimeZoneAnnotation? Annotations?
bool parseAnnotatedYearMonth(std::u16string_view s, RawParse& r) {
  Parser p(s);
  int32_t y, m;
  if (!p.dateSpecYearMonth(y, m)) return false;
  r.year = y;
  r.month = m;
  r.day = 1;
  return p.annotationsToEnd(r.tz, r.annotations, false);
}

// AnnotatedMonthDay ::: DateSpecMonthDay TimeZoneAnnotation? Annotations?
bool parseAnnotatedMonthDay(std::u16string_view s, RawParse& r) {
  Parser p(s);
  int32_t m, d;
  if (!p.dateSpecMonthDay(m, d)) return false;
  r.month = m;
  r.day = d;
  return p.annotationsToEnd(r.tz, r.annotations, false);
}

Throw invalidString(std::u16string_view s) { return ThrowRangeError("invalid ISO 8601 string: " + utf16ToUtf8(s)); }

}  // namespace

Result<ParsedISODateTime> ParseISODateTime(std::u16string_view isoString, uint32_t allowedFormats) {
  RawParse r;
  bool ok = false;
  bool yearMonthOrMonthDayForm = false;
  if (!ok && (allowedFormats & kDateTimeZoned)) {
    r = RawParse();
    ok = parseAnnotatedDateTime(isoString, true, false, r);
  }
  if (!ok && (allowedFormats & kDateTime)) {
    r = RawParse();
    ok = parseAnnotatedDateTime(isoString, false, false, r);
  }
  if (!ok && (allowedFormats & kInstant)) {
    r = RawParse();
    ok = parseInstant(isoString, r);
  }
  if (!ok && (allowedFormats & kTime)) {
    r = RawParse();
    ok = parseAnnotatedTime(isoString, r) || (r = RawParse(), parseAnnotatedDateTime(isoString, false, true, r));
  }
  if (!ok && (allowedFormats & kMonthDay)) {
    r = RawParse();
    ok = parseAnnotatedMonthDay(isoString, r);
    if (ok) yearMonthOrMonthDayForm = true;
    else ok = (r = RawParse(), parseAnnotatedDateTime(isoString, false, false, r));
  }
  if (!ok && (allowedFormats & kYearMonth)) {
    r = RawParse();
    ok = parseAnnotatedYearMonth(isoString, r);
    if (ok) yearMonthOrMonthDayForm = true;
    else ok = (r = RawParse(), parseAnnotatedDateTime(isoString, false, false, r));
  }
  if (!ok) return invalidString(isoString);

  // Annotations: the first u-ca wins; a second one is an error if either is critical; unknown critical
  // annotations are errors.
  ParsedISODateTime result;
  bool calendarWasCritical = false;
  for (const Annotation& a : r.annotations) {
    if (a.key == u"u-ca") {
      if (!result.calendar) {
        result.calendar = a.value;
        calendarWasCritical = a.critical;
      } else if (a.critical || calendarWasCritical) {
        return ThrowRangeError("conflicting calendar annotations");
      }
    } else if (a.critical) {
      return ThrowRangeError("unknown critical annotation");
    }
  }
  if (yearMonthOrMonthDayForm && result.calendar) {
    std::u16string lower = *result.calendar;
    for (char16_t& c : lower)
      if (c >= u'A' && c <= u'Z') c = char16_t(c + 32);
    if (lower != u"iso8601") return ThrowRangeError("a year-month or month-day string without a day/year must be ISO");
  }

  result.year = r.year;
  result.month = r.month;
  result.day = r.day;
  if (!IsValidISODate(r.year ? double(*r.year) : 1972.0, r.month, r.day)) return ThrowRangeError("invalid date in ISO string");
  if (r.time) {
    TimeParts t = r.time->second == 60 ? TimeParts{r.time->hour, r.time->minute, 59, r.time->fraction} : *r.time;
    int64_t f = t.fraction;
    result.time = CreateTimeRecord(t.hour, t.minute, t.second, int32_t(f / 1000000), int32_t(f / 1000 % 1000), int32_t(f % 1000));
  }
  result.timeZone = r.tz;
  return result;
}

// ---------------------------------------------------------------------------------------------------------
// Durations

Result<DurationFields> ParseTemporalDurationString(std::u16string_view s) {
  size_t i = 0, n = s.size();
  auto fail = [&]() -> Throw { return ThrowRangeError("invalid ISO 8601 duration string: " + utf16ToUtf8(s)); };
  double factor = 1;
  if (i < n && isSign(s[i])) {
    if (s[i] == u'-') factor = -1;
    i++;
  }
  if (i >= n || (s[i] != u'P' && s[i] != u'p')) return fail();
  i++;
  auto upper = [](char16_t c) { return c >= u'a' && c <= u'z' ? char16_t(c - 32) : c; };
  // Read DecimalDigits [TemporalDecimalFraction] followed by a designator.
  struct Part {
    double value;
    bool hasFraction;
    int64_t fractionNs;  // the fraction scaled to 9 digits
    char16_t designator;
  };
  auto readPart = [&](Part& part) -> bool {
    size_t start = i;
    while (i < n && isDigit(s[i])) i++;
    if (i == start) return false;
    std::u16string digitsText(s.substr(start, i - start));
    part.value = StringToNumber(digitsText);
    part.hasFraction = false;
    part.fractionNs = 0;
    if (i < n && (s[i] == u'.' || s[i] == u',')) {
      size_t fs = ++i;
      int64_t v = 0;
      while (i < n && isDigit(s[i]) && i - fs < 9) v = v * 10 + (s[i++] - u'0');
      size_t count = i - fs;
      if (count == 0 || (i < n && isDigit(s[i]))) return false;
      for (size_t k = count; k < 9; k++) v *= 10;
      part.hasFraction = true;
      part.fractionNs = v;
    }
    if (i >= n) return false;
    part.designator = upper(s[i++]);
    return true;
  };

  DurationFields d;
  bool any = false;
  // DurationDate: Y? M? W? D? in order, no fractions.
  int dateStage = 0;  // 0: none yet, 1: Y, 2: M, 3: W, 4: D
  while (i < n && s[i] != u'T' && s[i] != u't') {
    Part part;
    if (!readPart(part) || part.hasFraction) return fail();
    int stage = part.designator == u'Y' ? 1 : part.designator == u'M' ? 2 : part.designator == u'W' ? 3 : part.designator == u'D' ? 4 : 0;
    if (stage == 0 || stage <= dateStage) return fail();
    dateStage = stage;
    any = true;
    if (stage == 1) d.years = part.value;
    else if (stage == 2) d.months = part.value;
    else if (stage == 3) d.weeks = part.value;
    else d.days = part.value;
  }
  // DurationTime: T H? M? S? in order; only the last part may have a fraction.
  bool hasFraction = false;
  int64_t fractionNs = 0;
  Unit fractionUnit = Unit::Nanosecond;
  if (i < n) {
    i++;  // T
    int timeStage = 0;
    if (i >= n) return fail();
    while (i < n) {
      if (hasFraction) return fail();
      Part part;
      if (!readPart(part)) return fail();
      int stage = part.designator == u'H' ? 1 : part.designator == u'M' ? 2 : part.designator == u'S' ? 3 : 0;
      if (stage == 0 || stage <= timeStage) return fail();
      timeStage = stage;
      any = true;
      if (stage == 1) d.hours = part.value, fractionUnit = Unit::Hour;
      else if (stage == 2) d.minutes = part.value, fractionUnit = Unit::Minute;
      else d.seconds = part.value, fractionUnit = Unit::Second;
      if (part.hasFraction) {
        hasFraction = true;
        fractionNs = part.fractionNs;
      }
    }
  }
  if (!any) return fail();
  if (hasFraction) {
    // The exact nanoseconds of the fraction of the unit, distributed over the smaller units.
    Int128 ns = Int128(fractionNs) * (UnitLengthInNanoseconds(fractionUnit) / nsPerSecond);
    if (fractionUnit == Unit::Hour) {
      d.minutes = double(int64_t(ns / nsPerMinute));
      ns %= nsPerMinute;
    }
    if (fractionUnit == Unit::Hour || fractionUnit == Unit::Minute) {
      d.seconds = double(int64_t(ns / nsPerSecond));
      ns %= nsPerSecond;
    }
    d.milliseconds = double(int64_t(ns / 1000000));
    d.microseconds = double(int64_t(ns / 1000 % 1000));
    d.nanoseconds = double(int64_t(ns % 1000));
  }
  double* fields[] = {&d.years, &d.months, &d.weeks, &d.days, &d.hours, &d.minutes, &d.seconds, &d.milliseconds,
                      &d.microseconds, &d.nanoseconds};
  for (double* f : fields) *f = *f * factor + 0.0;
  return d;
}

// ---------------------------------------------------------------------------------------------------------
// Offsets and identifiers

Result<int64_t> ParseDateTimeUTCOffset(std::u16string_view offsetString) {
  Parser p(offsetString);
  if (!p.utcOffset(true) || !p.atEnd()) return ThrowRangeError("invalid UTC offset: " + utf16ToUtf8(offsetString));
  size_t i = 0;
  int64_t sign = offsetString[i++] == u'-' ? -1 : 1;
  auto two = [&]() {
    int64_t v = (offsetString[i] - u'0') * 10 + (offsetString[i + 1] - u'0');
    i += 2;
    return v;
  };
  int64_t hours = two(), minutes = 0, seconds = 0, fraction = 0;
  if (i < offsetString.size()) {
    if (offsetString[i] == u':') i++;
    minutes = two();
    if (i < offsetString.size()) {
      if (offsetString[i] == u':') i++;
      seconds = two();
      if (i < offsetString.size()) {
        i++;  // separator
        size_t count = 0;
        while (i < offsetString.size()) {
          fraction = fraction * 10 + (offsetString[i++] - u'0');
          count++;
        }
        for (; count < 9; count++) fraction *= 10;
      }
    }
  }
  return sign * (((hours * 60 + minutes) * 60 + seconds) * nsPerSecond + fraction);
}

bool IsUTCOffsetMinutePrecision(std::u16string_view s, int32_t* offsetMinutes) {
  Parser p(s);
  if (!p.utcOffset(false) || !p.atEnd()) return false;
  if (offsetMinutes) {
    int32_t sign = s[0] == u'-' ? -1 : 1;
    int32_t h = (s[1] - u'0') * 10 + (s[2] - u'0');
    int32_t m = 0;
    if (s.size() > 3) {
      size_t k = s[3] == u':' ? 4 : 3;
      m = (s[k] - u'0') * 10 + (s[k + 1] - u'0');
    }
    *offsetMinutes = sign * (h * 60 + m);
  }
  return true;
}

bool HasSubMinutePrecision(std::u16string_view offsetString) {
  Parser p(offsetString);
  bool sub = false;
  return p.utcOffset(true, &sub) && p.atEnd() && sub;
}

bool IsTimeZoneIANANameSyntax(std::u16string_view s) {
  Parser p(s);
  return p.tzIANAName() && p.atEnd();
}

bool IsAnnotationValueSyntax(std::u16string_view s) {
  Parser p(s);
  return p.annotationValue() && p.atEnd();
}

}  // namespace snowman::temporal
