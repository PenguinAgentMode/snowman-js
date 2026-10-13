// Time zones for Temporal: identifiers, UTC offsets, possible instants, transitions (time_zone.h).
#include "builtins/temporal/time_zone.h"

#include <time.h>

#include <algorithm>
#include <memory>

#include "runtime/bigint.h"
#include "runtime/errors.h"
#include "runtime/heap.h"

namespace snowman::temporal {

namespace {

// ---- the generated tables (tools/temporal/gen-tz.mjs) ----------------------------------------------------------

struct TzName {
  const char* name;
  int32_t zone;
};
struct TzZone {
  int32_t primaryName;  // index into kTzNames
  int32_t offsets;      // kTzOffsets[offsets .. offsets + offsetCount)
  int32_t offsetCount;
  int32_t bytesBegin, bytesEnd;  // the zone's transitions in the decoded kTzTransitions64
  int32_t transitionCount;
  int32_t rule;  // index into kTzRules, -1 if the zone has no annual rule
};
// One transition of an annual rule: in `month`, on day `day` (weekday < 0), else on the first `weekday` (0 =
// Sunday) on or after `day`, or the last `weekday` of the month (day 0); at `secs` seconds after local midnight
// in the offset before the transition; the new offset is `offset` (seconds).
struct TzRuleTransition {
  int32_t month, day, weekday, secs, offset;
};
// From `cut` (the start of a UTC year, epoch seconds) on, the zone follows the rule: transitions a then b each
// year.
struct TzRule {
  int64_t cut;
  TzRuleTransition a, b;
};

#include "builtins/temporal/tz_data.inc"

constexpr int32_t kNameCount = int32_t(sizeof(kTzNames) / sizeof(kTzNames[0]));
constexpr int32_t kZoneCount = int32_t(sizeof(kTzZones) / sizeof(kTzZones[0]));

// ---- integer helpers -----------------------------------------------------------------------------------------

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
EpochNanoseconds floorDiv128(EpochNanoseconds a, int64_t b) {
  EpochNanoseconds q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }

// Days from civil (proleptic Gregorian), exact for any int64-safe year.
int64_t daysFromCivil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2;
  const int64_t era = floorDiv(y, 400);
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
void civilFromDays(int64_t z, int64_t& y, int32_t& m, int32_t& d) {
  z += 719468;
  const int64_t era = floorDiv(z, 146097);
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  d = int32_t(doy - (153 * mp + 2) / 5 + 1);
  m = int32_t(mp < 10 ? mp + 3 : mp - 9);
  y = yoe + era * 400 + (m <= 2);
}
bool isLeapYear(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int32_t daysInMonth(int64_t y, int32_t m) {
  static const int32_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && isLeapYear(y) ? 29 : days[m - 1];
}
int32_t weekdayOf(int64_t epochDays) { return int32_t(floorMod(epochDays + 4, 7)); }  // 0 = Sunday
int64_t yearOfEpochSeconds(int64_t sec) {
  int64_t y;
  int32_t m, d;
  civilFromDays(floorDiv(sec, 86400), y, m, d);
  return y;
}

// BalanceISODateTime + GetUTCEpochNanoseconds of possibly unbalanced fields.
EpochNanoseconds utcEpochNs(int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute, int64_t second,
                            int64_t millisecond, int64_t microsecond, int64_t nanosecond) {
  const int64_t monthIndex = month - 1;
  const int64_t y = year + floorDiv(monthIndex, 12);
  const int64_t m = floorMod(monthIndex, 12) + 1;
  const EpochNanoseconds days = daysFromCivil(y, m, 1) + day - 1;
  EpochNanoseconds ns = days * nsPerDay;
  ns += EpochNanoseconds(hour) * nsPerHour + EpochNanoseconds(minute) * nsPerMinute +
        EpochNanoseconds(second) * nsPerSecond + EpochNanoseconds(millisecond) * 1'000'000 +
        EpochNanoseconds(microsecond) * 1'000 + nanosecond;
  return ns;
}
EpochNanoseconds utcEpochNs(const ISODateTime& t) {
  return utcEpochNs(t.isoDate.year, t.isoDate.month, t.isoDate.day, t.time.hour, t.time.minute, t.time.second,
                    t.time.millisecond, t.time.microsecond, t.time.nanosecond);
}

// GetISOPartsFromEpoch for any epoch ns (the balanced UTC date-time).
ISODateTime isoPartsFromEpoch(EpochNanoseconds ns) {
  const EpochNanoseconds days = floorDiv128(ns, nsPerDay);
  int64_t nsOfDay = int64_t(ns - days * nsPerDay);
  ISODateTime r;
  int64_t y;
  civilFromDays(int64_t(days), y, r.isoDate.month, r.isoDate.day);
  r.isoDate.year = int32_t(y);
  r.time.hour = int32_t(nsOfDay / nsPerHour);
  nsOfDay %= nsPerHour;
  r.time.minute = int32_t(nsOfDay / nsPerMinute);
  nsOfDay %= nsPerMinute;
  r.time.second = int32_t(nsOfDay / nsPerSecond);
  nsOfDay %= nsPerSecond;
  r.time.millisecond = int32_t(nsOfDay / 1'000'000);
  r.time.microsecond = int32_t(nsOfDay / 1'000 % 1'000);
  r.time.nanosecond = int32_t(nsOfDay % 1'000);
  return r;
}

// CheckISODaysRange: |ISODateToEpochDays| <= 10^8.
bool isoDaysInRange(const ISODate& date) {
  int64_t days = daysFromCivil(date.year, date.month, date.day);
  return days >= -100'000'000 && days <= 100'000'000;
}

// ---- decoded zones -------------------------------------------------------------------------------------------

struct Zone {
  int32_t initialOffset = 0;      // seconds, before the first transition
  std::vector<int64_t> times;     // epoch seconds, ascending
  std::vector<int32_t> offsets;   // the offset from times[i] on
  const TzRule* rule = nullptr;
  int64_t ruleStartYear = 0;
};

const std::vector<uint8_t>& transitionBytes() {
  static const std::vector<uint8_t> bytes = [] {
    std::vector<uint8_t> out;
    out.reserve(kTzTransitionBytes);
    uint32_t acc = 0;
    int bits = 0;
    for (const char* p = kTzTransitions64; *p && *p != '='; p++) {
      char c = *p;
      int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '+' ? 62 : 63;
      acc = (acc << 6) | uint32_t(v);
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.push_back(uint8_t(acc >> bits));
        acc &= (1u << bits) - 1;
      }
    }
    return out;
  }();
  return bytes;
}

const Zone& zoneData(int32_t zoneIndex) {
  static std::vector<std::unique_ptr<Zone>> cache(kZoneCount);
  std::unique_ptr<Zone>& slot = cache[zoneIndex];
  if (slot) return *slot;
  const TzZone& row = kTzZones[zoneIndex];
  auto zone = std::make_unique<Zone>();
  const int32_t* offsets = kTzOffsets + row.offsets;
  zone->initialOffset = offsets[0];
  zone->times.reserve(row.transitionCount);
  zone->offsets.reserve(row.transitionCount);
  const std::vector<uint8_t>& bytes = transitionBytes();
  int64_t t = kTzStart;
  for (int32_t i = row.bytesBegin; i < row.bytesEnd;) {
    uint64_t v = 0;
    int shift = 0;
    while (bytes[i] & 0x80) {
      v |= uint64_t(bytes[i++] & 0x7f) << shift;
      shift += 7;
    }
    v |= uint64_t(bytes[i++]) << shift;
    t += (v & 1) ? int64_t(v >> 1) : int64_t(v >> 1) * 60;
    zone->times.push_back(t);
    zone->offsets.push_back(offsets[bytes[i++]]);
  }
  if (row.rule >= 0) {
    zone->rule = &kTzRules[row.rule];
    zone->ruleStartYear = yearOfEpochSeconds(zone->rule->cut);
  }
  slot = std::move(zone);
  return *slot;
}

int32_t zoneOf(TimeZone timeZone) { return kTzNames[timeZone.id].zone; }

// The instant (epoch seconds) of a rule transition in year y.
int64_t ruleInstant(const TzRuleTransition& r, int64_t y, int32_t offsetBefore) {
  int64_t day;
  if (r.weekday < 0) {
    day = r.day;
  } else if (r.day == 0) {
    int32_t last = daysInMonth(y, r.month);
    int32_t wd = weekdayOf(daysFromCivil(y, r.month, last));
    day = last - (wd - r.weekday + 7) % 7;
  } else {
    int32_t wd = weekdayOf(daysFromCivil(y, r.month, r.day));
    day = r.day + (r.weekday - wd + 7) % 7;
  }
  return daysFromCivil(y, r.month, day) * 86400 + r.secs - offsetBefore;
}
int64_t ruleA(const TzRule& rule, int64_t y) { return ruleInstant(rule.a, y, rule.b.offset); }
int64_t ruleB(const TzRule& rule, int64_t y) { return ruleInstant(rule.b, y, rule.a.offset); }

int32_t offsetSecondsAt(const Zone& zone, int64_t sec) {
  if (zone.rule && sec >= zone.rule->cut) {
    int64_t y = yearOfEpochSeconds(sec);
    if (sec < ruleA(*zone.rule, y)) return zone.rule->b.offset;
    if (sec < ruleB(*zone.rule, y)) return zone.rule->a.offset;
    return zone.rule->b.offset;
  }
  auto it = std::upper_bound(zone.times.begin(), zone.times.end(), sec);
  if (it == zone.times.begin()) return zone.initialOffset;
  return zone.offsets[it - zone.times.begin() - 1];
}

// The first transition at a second > sec, if any.
std::optional<int64_t> nextTransitionSeconds(const Zone& zone, int64_t sec) {
  if (!zone.rule || sec < zone.rule->cut) {
    auto it = std::upper_bound(zone.times.begin(), zone.times.end(), sec);
    if (it != zone.times.end()) return *it;
    if (!zone.rule) return std::nullopt;
  }
  for (int64_t y = std::max(yearOfEpochSeconds(sec), zone.ruleStartYear);; y++) {
    int64_t a = ruleA(*zone.rule, y), b = ruleB(*zone.rule, y);
    if (a > sec) return a;
    if (b > sec) return b;
  }
}

// The last transition at a second < sec, if any.
std::optional<int64_t> previousTransitionSeconds(const Zone& zone, int64_t sec) {
  if (zone.rule && sec > zone.rule->cut) {
    for (int64_t y = yearOfEpochSeconds(sec); y >= zone.ruleStartYear; y--) {
      int64_t a = ruleA(*zone.rule, y), b = ruleB(*zone.rule, y);
      if (b < sec) return b;
      if (a < sec) return a;
    }
  }
  auto it = std::lower_bound(zone.times.begin(), zone.times.end(), sec);
  if (it == zone.times.begin()) return std::nullopt;
  return *(it - 1);
}

int32_t asciiLower(char16_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// ASCII-case-insensitive comparison of a candidate with a table name.
int compareFolded(std::u16string_view a, const char* b) {
  size_t i = 0;
  for (; i < a.size() && b[i]; i++) {
    int32_t x = asciiLower(a[i]), y = asciiLower(char16_t(uint8_t(b[i])));
    if (x != y) return x < y ? -1 : 1;
  }
  if (i == a.size()) return b[i] ? -1 : 0;
  return 1;
}

std::u16string ascii(const char* s) { return std::u16string(s, s + std::char_traits<char>::length(s)); }

void appendTwoDigits(std::u16string& out, int64_t n) {
  out.push_back(char16_t('0' + n / 10));
  out.push_back(char16_t('0' + n % 10));
}

bool isDigit(char16_t c) { return c >= '0' && c <= '9'; }
bool isAlpha(char16_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// UTCOffset[~SubMinutePrecision]: ±HH, ±HH:MM, ±HHMM. Minutes, or nullopt.
std::optional<int32_t> parseMinuteOffset(std::u16string_view s) {
  if (s.size() != 3 && s.size() != 5 && s.size() != 6) return std::nullopt;
  if (s[0] != '+' && s[0] != '-') return std::nullopt;
  if (!isDigit(s[1]) || !isDigit(s[2])) return std::nullopt;
  int32_t hour = (s[1] - '0') * 10 + (s[2] - '0');
  if (hour > 23) return std::nullopt;
  int32_t minute = 0;
  if (s.size() > 3) {
    std::u16string_view mm = s.substr(3);
    if (mm.size() == 3) {
      if (mm[0] != ':') return std::nullopt;
      mm = mm.substr(1);
    }
    if (!isDigit(mm[0]) || !isDigit(mm[1]) || mm[0] > '5') return std::nullopt;
    minute = (mm[0] - '0') * 10 + (mm[1] - '0');
  }
  int32_t total = hour * 60 + minute;
  return s[0] == '-' ? -total : total;
}

// TimeZoneIANAName: components separated by '/', each TZLeadingChar TZChar*.
bool isTimeZoneIANAName(std::u16string_view s) {
  if (s.empty()) return false;
  bool atStart = true;
  for (char16_t c : s) {
    if (c == '/') {
      if (atStart) return false;
      atStart = true;
      continue;
    }
    bool leading = isAlpha(c) || c == '.' || c == '_';
    if (atStart) {
      if (!leading) return false;
      atStart = false;
    } else if (!leading && !isDigit(c) && c != '-' && c != '+') {
      return false;
    }
  }
  return !atStart;
}

int32_t utcNameIndex() {
  static const int32_t index = [] {
    auto tz = GetAvailableNamedTimeZoneIdentifier(u"UTC");
    return tz ? tz->id : 0;
  }();
  return index;
}

}  // namespace

// ---- BigInt conversions ----------------------------------------------------------------------------------------

std::optional<EpochNanoseconds> BigIntToEpochNanoseconds(const BigInt* x) {
  const std::vector<uint32_t>& limbs = x->magnitude();
  if (limbs.size() > 4 || (limbs.size() == 4 && (limbs[3] & 0x80000000u))) return std::nullopt;
  unsigned __int128 magnitude = 0;
  for (size_t i = limbs.size(); i-- > 0;) magnitude = (magnitude << 32) | limbs[i];
  EpochNanoseconds value = EpochNanoseconds(magnitude);
  return x->isNegative() ? -value : value;
}

BigInt* EpochNanosecondsToBigInt(EpochNanoseconds ns) {
  bool negative = ns < 0;
  unsigned __int128 magnitude = negative ? (unsigned __int128)(-ns) : (unsigned __int128)ns;
  std::vector<uint32_t> limbs;
  while (magnitude) {
    limbs.push_back(uint32_t(magnitude & 0xffffffffu));
    magnitude >>= 32;
  }
  return make<BigInt>(negative, std::move(limbs));
}

// ---- identifiers -----------------------------------------------------------------------------------------------

TimeZone TimeZone::UTC() { return TimeZone{utcNameIndex(), 0}; }

bool TimeZone::isUTC() const { return !isOffset() && kTzNames[id].zone == kTzNames[utcNameIndex()].zone; }

std::optional<TimeZoneIdentifierParseRecord> ParseTimeZoneIdentifier(std::u16string_view identifier) {
  if (auto minutes = parseMinuteOffset(identifier)) return TimeZoneIdentifierParseRecord{u"", minutes};
  if (isTimeZoneIANAName(identifier)) return TimeZoneIdentifierParseRecord{std::u16string(identifier), std::nullopt};
  return std::nullopt;
}

std::optional<TimeZone> GetAvailableNamedTimeZoneIdentifier(std::u16string_view name) {
  int32_t lo = 0, hi = kNameCount;
  while (lo < hi) {
    int32_t mid = (lo + hi) / 2;
    int c = compareFolded(name, kTzNames[mid].name);
    if (c == 0) return TimeZone{mid, 0};
    if (c < 0) hi = mid;
    else lo = mid + 1;
  }
  return std::nullopt;
}

Result<TimeZone> TimeZoneFromParseRecord(const TimeZoneIdentifierParseRecord& record) {
  if (record.offsetMinutes) return TimeZone::Offset(*record.offsetMinutes);
  auto tz = GetAvailableNamedTimeZoneIdentifier(record.name);
  if (!tz) return ThrowRangeError("unknown time zone");
  return *tz;
}

std::u16string TimeZoneIdentifier(TimeZone timeZone) {
  if (timeZone.isOffset()) return FormatOffsetTimeZoneIdentifier(timeZone.offsetMinutes);
  return ascii(kTzNames[timeZone.id].name);
}

std::u16string PrimaryTimeZoneIdentifier(TimeZone timeZone) {
  if (timeZone.isOffset()) return FormatOffsetTimeZoneIdentifier(timeZone.offsetMinutes);
  return ascii(kTzNames[kTzZones[zoneOf(timeZone)].primaryName].name);
}

bool TimeZoneEquals(TimeZone one, TimeZone two) {
  if (one.isOffset() || two.isOffset()) {
    return one.isOffset() && two.isOffset() && one.offsetMinutes == two.offsetMinutes;
  }
  return zoneOf(one) == zoneOf(two);
}

std::vector<std::u16string> AvailableNamedTimeZoneIdentifiers() {
  std::vector<std::u16string> out;
  for (int32_t i = 0; i < kNameCount; i++) out.push_back(ascii(kTzNames[i].name));
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::u16string> AvailablePrimaryTimeZoneIdentifiers() {
  std::vector<std::u16string> out;
  for (int32_t i = 0; i < kZoneCount; i++) out.push_back(ascii(kTzNames[kTzZones[i].primaryName].name));
  std::sort(out.begin(), out.end());
  return out;
}

// ---- formatting ------------------------------------------------------------------------------------------------

std::u16string FormatOffsetTimeZoneIdentifier(int32_t offsetMinutes, bool separated) {
  std::u16string out(1, offsetMinutes >= 0 ? u'+' : u'-');
  int32_t absolute = offsetMinutes < 0 ? -offsetMinutes : offsetMinutes;
  appendTwoDigits(out, absolute / 60);
  if (separated) out.push_back(':');
  appendTwoDigits(out, absolute % 60);
  return out;
}

std::u16string FormatUTCOffsetNanoseconds(int64_t offsetNanoseconds) {
  std::u16string out(1, offsetNanoseconds >= 0 ? u'+' : u'-');
  int64_t absolute = offsetNanoseconds < 0 ? -offsetNanoseconds : offsetNanoseconds;
  appendTwoDigits(out, absolute / nsPerHour);
  out.push_back(':');
  appendTwoDigits(out, absolute / nsPerMinute % 60);
  int64_t second = absolute / nsPerSecond % 60;
  int64_t subSecond = absolute % nsPerSecond;
  if (second == 0 && subSecond == 0) return out;
  out.push_back(':');
  appendTwoDigits(out, second);
  if (subSecond != 0) {
    std::u16string digits;
    for (int64_t scale = 100'000'000; scale > 0 && subSecond != 0; scale /= 10) {
      digits.push_back(char16_t('0' + subSecond / scale));
      subSecond %= scale;
    }
    out.push_back('.');
    out += digits;
  }
  return out;
}

std::u16string FormatDateTimeUTCOffsetRounded(int64_t offsetNanoseconds) {
  // RoundNumberToIncrement(offsetNanoseconds, 60 × 10^9, half-expand)
  int64_t q = offsetNanoseconds / nsPerMinute, r = offsetNanoseconds % nsPerMinute;
  if (r >= nsPerMinute / 2) q++;
  else if (r <= -nsPerMinute / 2) q--;
  return FormatOffsetTimeZoneIdentifier(int32_t(q));
}

// ---- offsets and transitions -----------------------------------------------------------------------------------

int64_t GetNamedTimeZoneOffsetNanoseconds(TimeZone timeZone, EpochNanoseconds epochNs) {
  const Zone& zone = zoneData(zoneOf(timeZone));
  return int64_t(offsetSecondsAt(zone, int64_t(floorDiv128(epochNs, nsPerSecond)))) * nsPerSecond;
}

int64_t GetOffsetNanosecondsFor(TimeZone timeZone, EpochNanoseconds epochNs) {
  if (timeZone.isOffset()) return int64_t(timeZone.offsetMinutes) * nsPerMinute;
  return GetNamedTimeZoneOffsetNanoseconds(timeZone, epochNs);
}

std::optional<EpochNanoseconds> GetNamedTimeZoneNextTransition(TimeZone timeZone, EpochNanoseconds epochNs) {
  const Zone& zone = zoneData(zoneOf(timeZone));
  // A transition at second s begins at s × 10^9 > epochNs  <=>  s > floor(epochNs / 10^9).
  auto next = nextTransitionSeconds(zone, int64_t(floorDiv128(epochNs, nsPerSecond)));
  if (!next) return std::nullopt;
  EpochNanoseconds ns = EpochNanoseconds(*next) * nsPerSecond;
  if (ns > nsMaxInstant) return std::nullopt;
  return ns;
}

std::optional<EpochNanoseconds> GetNamedTimeZonePreviousTransition(TimeZone timeZone, EpochNanoseconds epochNs) {
  const Zone& zone = zoneData(zoneOf(timeZone));
  // s × 10^9 < epochNs  <=>  s < ceil(epochNs / 10^9).
  int64_t ceilSeconds = int64_t(-floorDiv128(-epochNs, nsPerSecond));
  auto previous = previousTransitionSeconds(zone, ceilSeconds);
  if (!previous) return std::nullopt;
  EpochNanoseconds ns = EpochNanoseconds(*previous) * nsPerSecond;
  if (ns < nsMinInstant) return std::nullopt;
  return ns;
}

ISODateTime GetISODateTimeFor(TimeZone timeZone, EpochNanoseconds epochNs) {
  return isoPartsFromEpoch(epochNs + GetOffsetNanosecondsFor(timeZone, epochNs));
}

std::vector<EpochNanoseconds> GetNamedTimeZoneEpochNanoseconds(TimeZone timeZone, const ISODateTime& isoDateTime) {
  const Zone& zone = zoneData(zoneOf(timeZone));
  const EpochNanoseconds local = utcEpochNs(isoDateTime);
  // Every offset in effect within a day of `local` (|offset| < 24 h) is a candidate.
  const int64_t sec = int64_t(floorDiv128(local, nsPerSecond));
  std::vector<int32_t> offsets{offsetSecondsAt(zone, sec - 86400)};
  for (int64_t t = sec - 86400;;) {
    auto next = nextTransitionSeconds(zone, t);
    if (!next || *next > sec + 86400) break;
    t = *next;
    int32_t o = offsetSecondsAt(zone, t);
    if (std::find(offsets.begin(), offsets.end(), o) == offsets.end()) offsets.push_back(o);
  }
  std::vector<EpochNanoseconds> result;
  for (int32_t o : offsets) {
    EpochNanoseconds candidate = local - EpochNanoseconds(o) * nsPerSecond;
    if (offsetSecondsAt(zone, int64_t(floorDiv128(candidate, nsPerSecond))) == o) result.push_back(candidate);
  }
  std::sort(result.begin(), result.end());
  return result;
}

Result<std::vector<EpochNanoseconds>> GetPossibleEpochNanoseconds(TimeZone timeZone, const ISODateTime& isoDateTime) {
  std::vector<EpochNanoseconds> possible;
  if (timeZone.isOffset()) {
    const ISODateTime balanced = isoPartsFromEpoch(utcEpochNs(isoDateTime) - EpochNanoseconds(timeZone.offsetMinutes) * nsPerMinute);
    if (!isoDaysInRange(balanced.isoDate)) return ThrowRangeError("date-time outside the representable range");
    possible.push_back(utcEpochNs(balanced));
  } else {
    possible = GetNamedTimeZoneEpochNanoseconds(timeZone, isoDateTime);
  }
  for (EpochNanoseconds ns : possible) {
    if (!IsValidEpochNanoseconds(ns)) return ThrowRangeError("date-time outside the representable range");
  }
  return possible;
}

Result<EpochNanoseconds> DisambiguatePossibleEpochNanoseconds(const std::vector<EpochNanoseconds>& possibleEpochNs,
                                                             TimeZone timeZone, const ISODateTime& isoDateTime,
                                                             Disambiguation disambiguation) {
  const size_t n = possibleEpochNs.size();
  if (n == 1) return possibleEpochNs[0];
  if (n != 0) {
    if (disambiguation == Disambiguation::Earlier || disambiguation == Disambiguation::Compatible) {
      return possibleEpochNs[0];
    }
    if (disambiguation == Disambiguation::Later) return possibleEpochNs[n - 1];
    return ThrowRangeError("the wall-clock time is ambiguous in this time zone (disambiguation: reject)");
  }
  if (disambiguation == Disambiguation::Reject) {
    return ThrowRangeError("the wall-clock time does not exist in this time zone (disambiguation: reject)");
  }
  // A gap: the offsets a day before and after (as the reference implementation does) give its length.
  const EpochNanoseconds utc = utcEpochNs(isoDateTime);
  const EpochNanoseconds dayBefore = utc - nsPerDay, dayAfter = utc + nsPerDay;
  if (!IsValidEpochNanoseconds(dayBefore) || !IsValidEpochNanoseconds(dayAfter)) {
    return ThrowRangeError("date-time outside the representable range");
  }
  const int64_t nanoseconds = GetOffsetNanosecondsFor(timeZone, dayAfter) - GetOffsetNanosecondsFor(timeZone, dayBefore);
  const ISODateTime shifted = isoPartsFromEpoch(
      utc + (disambiguation == Disambiguation::Earlier ? -nanoseconds : nanoseconds));
  std::vector<EpochNanoseconds> possible = TRY(GetPossibleEpochNanoseconds(timeZone, shifted));
  if (possible.empty()) return ThrowRangeError("date-time outside the representable range");
  return disambiguation == Disambiguation::Earlier ? possible.front() : possible.back();
}

Result<EpochNanoseconds> GetEpochNanosecondsFor(TimeZone timeZone, const ISODateTime& isoDateTime,
                                               Disambiguation disambiguation) {
  std::vector<EpochNanoseconds> possible = TRY(GetPossibleEpochNanoseconds(timeZone, isoDateTime));
  return DisambiguatePossibleEpochNanoseconds(possible, timeZone, isoDateTime, disambiguation);
}

Result<EpochNanoseconds> GetStartOfDay(TimeZone timeZone, ISODate isoDate) {
  ISODateTime midnight{isoDate, TimeRecord{}};
  std::vector<EpochNanoseconds> possible = TRY(GetPossibleEpochNanoseconds(timeZone, midnight));
  if (!possible.empty()) return possible[0];
  // Midnight is in a gap: the start of the day is the transition that skips it.
  const EpochNanoseconds dayBefore = utcEpochNs(midnight) - nsPerDay;
  if (!IsValidEpochNanoseconds(dayBefore)) return ThrowRangeError("date-time outside the representable range");
  auto transition = GetNamedTimeZoneNextTransition(timeZone, dayBefore);
  if (!transition) return ThrowRangeError("date-time outside the representable range");
  return *transition;
}

// ---- the host --------------------------------------------------------------------------------------------------

TimeZone SystemTimeZoneIdentifier() { return TimeZone::UTC(); }

EpochNanoseconds SystemUTCEpochNanoseconds() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  EpochNanoseconds ns = EpochNanoseconds(ts.tv_sec) * nsPerSecond + ts.tv_nsec;
  return std::clamp(ns, nsMinInstant, nsMaxInstant);
}

}  // namespace snowman::temporal
