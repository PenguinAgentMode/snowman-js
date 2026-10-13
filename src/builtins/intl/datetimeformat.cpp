// Intl.DateTimeFormat (ECMA-402 11): CreateDateTimeFormat, a DateTimePatternGenerator-style format matcher over the
// CLDR availableFormats (date_data.inc), FormatDateTimePattern, formatToParts and formatRange. Gregorian arithmetic;
// time zones: UTC and offsets (named zones are accepted and canonicalized, formatted at offset 0 for now).
#include <algorithm>
#include <cmath>

#include "builtins/intl/hooks.h"
#include "builtins/intl/intl.h"
#include "builtins/intl/services.h"
#include "builtins/temporal/temporal.h"
#include "runtime/symbol.h"

namespace snowman::intl {

namespace {

struct DataEntry {
  const char* key;
  const char16_t* value;
};
struct AliasEntry {
  const char* from;
  const char* to;
};
#include "builtins/intl/date_data.inc"

enum DateField {
  FMonthsFormatAbbr = 0, FDaysFormatAbbr = 6, FEraAbbr = 14, FDayPeriodsAbbr = 17, FDateFormats = 20, FTimeFormats = 21,
  FDateTimeFormats = 22, FAtTimeFormats = 23, FAvailable = 24, FAppendItems = 25, FZoneFormats = 26, FDayPeriodRules = 27,
  FUtcNames = 28, FIntervals = 29, FFieldNames = 30
};

template <size_t N>
const char16_t* findEntry(const DataEntry (&table)[N], const std::string& key) {
  const DataEntry* end = table + N;
  const DataEntry* it = std::lower_bound(table, end, key, [](const DataEntry& e, const std::string& k) { return std::string_view(e.key) < k; });
  return it != end && key == it->key ? it->value : nullptr;
}

std::u16string_view dateField(const std::string& locale, int field) {
  std::string suffix = "/" + std::to_string(field);
  for (const std::string& c : data::lookupChain(locale))
    if (const char16_t* v = findEntry(kDateFields, c + suffix)) return v;
  return findEntry(kDateFields, suffix);
}

std::vector<std::u16string_view> items(std::u16string_view s) {
  std::vector<std::u16string_view> out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(u'\x1e', start);
    out.push_back(s.substr(start, pos == std::u16string_view::npos ? std::u16string_view::npos : pos - start));
    if (pos == std::u16string_view::npos) return out;
    start = pos + 1;
  }
}

// ---- Gregorian arithmetic ----

int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// Days since 1970-01-01 of a proleptic Gregorian date (Hinnant's days_from_civil).
int64_t daysFromCivil(int64_t y, int m, int d) {
  y -= m <= 2;
  int64_t era = floorDiv(y, 400);
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

struct LocalTime {
  int64_t year;
  int month, day, weekday, hour, minute, second, millisecond;
};

LocalTime toLocalTime(double epochMs) {
  int64_t ms = int64_t(epochMs);
  int64_t days = floorDiv(ms, 86400000);
  int64_t msOfDay = ms - days * 86400000;
  LocalTime t;
  int64_t z = days + 719468;
  int64_t era = floorDiv(z, 146097);
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  t.day = int(doy - (153 * mp + 2) / 5 + 1);
  t.month = int(mp < 10 ? mp + 3 : mp - 9);
  t.year = y + (t.month <= 2);
  t.weekday = int(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday
  t.hour = int(msOfDay / 3600000);
  t.minute = int(msOfDay / 60000 % 60);
  t.second = int(msOfDay / 1000 % 60);
  t.millisecond = int(msOfDay % 1000);
  return t;
}

// ---- patterns ----

struct Token {
  bool field;
  char16_t ch;
  int count;
  std::u16string text;  // literal text
};

bool isPatternLetter(char16_t c) { return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z'); }

std::vector<Token> parsePattern(std::u16string_view p) {
  std::vector<Token> out;
  auto literal = [&](char16_t c) {
    if (out.empty() || out.back().field) out.push_back({false, 0, 0, {}});
    out.back().text.push_back(c);
  };
  for (size_t i = 0; i < p.size();) {
    char16_t c = p[i];
    if (c == u'\'') {
      if (i + 1 < p.size() && p[i + 1] == u'\'') {
        literal(u'\'');
        i += 2;
        continue;
      }
      size_t j = i + 1;
      while (j < p.size()) {
        if (p[j] == u'\'') {
          if (j + 1 < p.size() && p[j + 1] == u'\'') {
            literal(u'\'');
            j += 2;
            continue;
          }
          break;
        }
        literal(p[j++]);
      }
      i = j + 1;
    } else if (isPatternLetter(c)) {
      size_t j = i;
      while (j < p.size() && p[j] == c) j++;
      out.push_back({true, c, int(j - i), {}});
      i = j;
    } else {
      literal(c);
      i++;
    }
  }
  return out;
}

std::u16string patternString(const std::vector<Token>& tokens) {
  std::u16string out;
  for (const Token& t : tokens) {
    if (t.field) {
      out.append(size_t(t.count), t.ch);
    } else {
      bool quote = std::any_of(t.text.begin(), t.text.end(), [](char16_t c) { return isPatternLetter(c) || c == u'\''; });
      if (quote) out.push_back(u'\'');
      for (char16_t c : t.text) {
        if (c == u'\'') out += u"''";
        else out.push_back(c);
      }
      if (quote) out.push_back(u'\'');
    }
  }
  return out;
}

// The DateTimePatternGenerator field types.
enum Field { Era, Year, Quarter, Month, Week, Weekday, Day, DayPeriod, Hour, Minute, Second, Fraction, Zone, kFieldCount };
constexpr unsigned kDateMask = (1u << Era) | (1u << Year) | (1u << Quarter) | (1u << Month) | (1u << Week) | (1u << Weekday) | (1u << Day);
constexpr unsigned kTimeMask = (1u << DayPeriod) | (1u << Hour) | (1u << Minute) | (1u << Second) | (1u << Fraction) | (1u << Zone);

int fieldOf(char16_t c) {
  switch (c) {
    case u'G': return Era;
    case u'y': case u'Y': case u'u': case u'U': case u'r': return Year;
    case u'Q': case u'q': return Quarter;
    case u'M': case u'L': return Month;
    case u'w': case u'W': return Week;
    case u'E': case u'c': case u'e': return Weekday;
    case u'd': case u'D': case u'F': case u'g': return Day;
    case u'a': case u'b': case u'B': return DayPeriod;
    case u'h': case u'H': case u'K': case u'k': case u'j': case u'J': case u'C': return Hour;
    case u'm': return Minute;
    case u's': return Second;
    case u'S': case u'A': return Fraction;
    case u'z': case u'Z': case u'O': case u'v': case u'V': case u'X': case u'x': return Zone;
    default: return -1;
  }
}

constexpr int kNumeric = 0x100, kNarrow = -0x101, kShort = -0x102, kLong = -0x103, kShorter = -0x104, kDelta = 0x10;

// The type value of a field (ICU dtTypes): numeric types are positive and include the length.
int typeValue(char16_t c, int len) {
  auto text = [&](int delta) { return len >= 6 ? kShorter - delta : len == 5 ? kNarrow - delta : len == 4 ? kLong - delta : kShort - delta; };
  switch (c) {
    case u'G': return text(0);
    case u'y': return kNumeric + len;
    case u'Y': return kNumeric + kDelta + len;
    case u'u': case u'U': case u'r': return kNumeric + 2 * kDelta + len;
    case u'Q': return len <= 2 ? kNumeric + len : text(0);
    case u'q': return len <= 2 ? kNumeric + kDelta + len : text(kDelta);
    case u'M': return len <= 2 ? kNumeric + len : text(0);
    case u'L': return len <= 2 ? kNumeric + kDelta + len : text(kDelta);
    case u'w': case u'W': return kNumeric + len;
    case u'E': return text(0);
    case u'c': return len <= 2 ? kNumeric + 2 * kDelta + len : text(2 * kDelta);
    case u'e': return len <= 2 ? kNumeric + kDelta + len : text(kDelta);
    case u'd': return kNumeric + len;
    case u'D': case u'F': case u'g': return kNumeric + kDelta + len;
    case u'a': return text(0);
    case u'b': return text(kDelta);
    case u'B': return text(2 * kDelta);
    case u'h': return kNumeric + len;
    case u'K': return kNumeric + kDelta + len;
    case u'H': return kNumeric + 10 * kDelta + len;
    case u'k': return kNumeric + 11 * kDelta + len;
    case u'm': return kNumeric + len;
    case u's': return kNumeric + len;
    case u'S': return kNumeric + kDelta + len;
    case u'z': return text(0);
    case u'v': return text(2 * kDelta);
    case u'O': return text(3 * kDelta);
    case u'Z': return text(4 * kDelta);
    case u'V': return text(5 * kDelta);
    case u'X': case u'x': return text(6 * kDelta);
    default: return 0;
  }
}

struct Skeleton {
  char16_t ch[kFieldCount] = {};
  int len[kFieldCount] = {};
  int type[kFieldCount] = {};
  unsigned mask() const {
    unsigned m = 0;
    for (int i = 0; i < kFieldCount; i++)
      if (ch[i]) m |= 1u << i;
    return m;
  }
  void set(char16_t c, int n) {
    int f = fieldOf(c);
    if (f < 0) return;
    ch[f] = c;
    len[f] = n;
    type[f] = typeValue(c, n);
  }
  void clear(int f) { ch[f] = 0, len[f] = 0, type[f] = 0; }
  // As ICU's DateTimeMatcher: a 12-hour skeleton without a day period gets "a"; a 24-hour one loses its day period.
  void normalizeDayPeriod() {
    if (!ch[Hour]) return;
    if (ch[Hour] == u'h' || ch[Hour] == u'K') {
      if (!ch[DayPeriod]) set(u'a', 1);
    } else {
      clear(DayPeriod);
    }
  }
};

Skeleton skeletonOf(std::u16string_view s) {
  Skeleton k;
  for (const Token& t : parsePattern(s))
    if (t.field) k.set(t.ch, t.count);
  k.normalizeDayPeriod();
  return k;
}

struct Distance {
  int value;
  unsigned missing, extra;
};

Distance distance(const Skeleton& req, const Skeleton& cand, unsigned includeMask) {
  Distance d{0, 0, 0};
  for (int i = 0; i < kFieldCount; i++) {
    int mine = (includeMask & (1u << i)) ? req.type[i] : 0;
    int other = cand.type[i];
    if (mine == other) continue;
    if (mine == 0) {
      d.value += 0x10000;
      d.extra |= 1u << i;
    } else if (other == 0) {
      d.value += 0x1000;
      d.missing |= 1u << i;
    } else {
      d.value += std::abs(mine - other);
    }
  }
  return d;
}

struct Available {
  Skeleton skeleton;
  std::u16string_view key, pattern;
};

// A locale's pattern generator data.
struct Generator {
  std::string locale;
  std::vector<Available> available;
  std::vector<std::pair<std::u16string_view, std::u16string_view>> appendItems;
  std::u16string_view atTime[4];
  std::vector<std::u16string_view> fieldNames;  // by Field (dayperiod and fraction have none)
  char16_t decimal = u'.';
  char16_t defaultHourChar = u'h';
};

const Generator& generatorFor(const std::string& locale) {
  static std::vector<std::unique_ptr<Generator>> cache;
  for (const auto& g : cache)
    if (g->locale == locale) return *g;
  auto g = std::make_unique<Generator>();
  g->locale = locale;
  // ICU's canonical items (one per field), then the locale's availableFormats (replacing equal skeletons). Ties
  // in matching go to the candidate whose leading field is the larger one (year before weekday), as ICU's do.
  static const char16_t* const canonical[] = {u"G", u"y", u"Q", u"M", u"w", u"W", u"E", u"d", u"a", u"H", u"m", u"s", u"S", u"v"};
  for (const char16_t* c : canonical) g->available.push_back({skeletonOf(c), c, c});
  for (std::u16string_view item : items(dateField(locale, FAvailable))) {
    size_t eq = item.find(u'=');
    if (eq == std::u16string_view::npos) continue;
    std::u16string_view key = item.substr(0, eq);
    auto same = std::find_if(g->available.begin(), g->available.end(), [&](const Available& a) { return a.key == key; });
    if (same != g->available.end()) same->pattern = item.substr(eq + 1);
    else g->available.push_back({skeletonOf(key), key, item.substr(eq + 1)});
  }
  auto rank = [](const Available& a) {
    for (int f = 0; f < kFieldCount; f++)
      if (a.skeleton.ch[f]) return f;
    return int(kFieldCount);
  };
  std::stable_sort(g->available.begin(), g->available.end(), [&](const Available& a, const Available& b) { return rank(a) < rank(b); });
  for (std::u16string_view item : items(dateField(locale, FAppendItems))) {
    size_t eq = item.find(u'=');
    if (eq != std::u16string_view::npos) g->appendItems.emplace_back(item.substr(0, eq), item.substr(eq + 1));
  }
  std::vector<std::u16string_view> names = items(dateField(locale, FFieldNames));
  names.resize(12);
  // Era Year Quarter Month Week Weekday Day DayPeriod Hour Minute Second Fraction Zone
  g->fieldNames = {names[0], names[1], names[2], names[3], names[4], names[5], names[6], names[7], names[8], names[9], names[10], {}, names[11]};
  std::vector<std::u16string_view> at = items(dateField(locale, FAtTimeFormats));
  for (int i = 0; i < 4 && i < int(at.size()); i++) g->atTime[i] = at[size_t(i)];
  std::string hc = data::defaultHourCycle(locale);
  g->defaultHourChar = hc == "h11" ? u'K' : hc == "h12" ? u'h' : hc == "h24" ? u'k' : u'H';
  cache.push_back(std::move(g));
  return *cache.back();
}

struct Best {
  const Available* match = nullptr;
  Distance distance{0x7fffffff, 0, 0};
};

Best getBestRaw(const Generator& g, const Skeleton& req, unsigned includeMask) {
  Best best;
  for (const Available& a : g.available) {
    Distance d = distance(req, a.skeleton, includeMask);
    if (d.value < best.distance.value) {
      best.match = &a;
      best.distance = d;
      if (d.value == 0) break;
    }
  }
  return best;
}

// ICU adjustFieldTypes: widen or narrow the fields of a matched pattern to the requested lengths.
std::vector<Token> adjustFieldTypes(const std::vector<Token>& pattern, const Skeleton& req, const Skeleton* matched, unsigned mask,
                                    char16_t decimal) {
  std::vector<Token> out;
  for (const Token& t : pattern) {
    if (!t.field) {
      out.push_back(t);
      continue;
    }
    int f = fieldOf(t.ch);
    Token field = t;
    if (f >= 0 && f != Fraction && (mask & (1u << f)) && req.ch[f]) {
      int reqLen = req.len[f];
      int adjLen = reqLen;
      if (f == Minute || f == Second) {
        adjLen = t.count;
      } else if (matched && matched->ch[f]) {
        bool patNumeric = typeValue(t.ch, t.count) > 0, skelNumeric = matched->type[f] > 0;
        if (matched->len[f] == reqLen || patNumeric != skelNumeric) adjLen = t.count;
      }
      char16_t c = (f != Hour && f != Month && f != Weekday && f != Year) ? req.ch[f] : t.ch;
      if (f == Hour) c = req.ch[f];
      field.ch = c;
      field.count = adjLen;
    }
    if (f == Fraction && req.ch[Fraction]) field.count = req.len[Fraction];
    out.push_back(field);
    if (f == Second && req.ch[Fraction] && (mask & (1u << Fraction))) {
      out.push_back({false, 0, 0, std::u16string(1, decimal)});
      out.push_back({true, u'S', req.len[Fraction], {}});
    }
  }
  return out;
}

int topBit(unsigned m) {
  int top = -1;
  for (int i = 0; i < kFieldCount; i++)
    if (m & (1u << i)) top = i;
  return top;
}

std::u16string_view appendItemFor(const Generator& g, int field) {
  static const char16_t* const names[] = {u"Era", u"Year", u"Quarter", u"Month", u"Week", u"Day-Of-Week", u"Day", u"",
                                          u"Hour", u"Minute", u"Second", u"", u"Timezone"};
  for (const auto& [k, v] : g.appendItems)
    if (k == names[field]) return v;
  return u"{0} {1}";
}

std::u16string formatPattern(std::u16string_view tmpl, std::u16string_view a, std::u16string_view b, std::u16string_view c = {}) {
  std::u16string out;
  for (size_t i = 0; i < tmpl.size(); i++) {
    if (tmpl[i] == u'{' && i + 2 < tmpl.size() && tmpl[i + 2] == u'}') {
      char16_t n = tmpl[i + 1];
      out += n == u'0' ? a : n == u'1' ? b : c;
      i += 2;
    } else {
      out.push_back(tmpl[i]);
    }
  }
  return out;
}

std::u16string getBestAppending(const Generator& g, const Skeleton& req, unsigned missingFields) {
  if (!missingFields) return {};
  Best best = getBestRaw(g, req, missingFields);
  std::vector<Token> result;
  if (best.match) result = adjustFieldTypes(parsePattern(best.match->pattern), req, &best.match->skeleton, missingFields, g.decimal);
  std::u16string pattern = patternString(result);
  unsigned missing = best.match ? best.distance.missing : missingFields;
  // Fractional seconds go with the seconds; alone they are their own field.
  if ((missing & (1u << Fraction)) && (missingFields & (1u << Second)) == 0) {
    std::u16string fraction(size_t(req.len[Fraction]), u'S');
    pattern = pattern.empty() ? fraction : formatPattern(u"{0} {1}", pattern, fraction);
    missing &= ~(1u << Fraction);
  }
  missing &= ~(1u << Fraction);
  for (int guard = 0; missing && guard < kFieldCount; guard++) {
    Best t = getBestRaw(g, req, missing);
    if (!t.match) break;
    unsigned found = missing & ~t.distance.missing;
    if (!found) break;
    std::u16string temp = patternString(adjustFieldTypes(parsePattern(t.match->pattern), req, &t.match->skeleton, missing, g.decimal));
    int top = topBit(found);
    std::u16string name = u"'" + std::u16string(g.fieldNames[size_t(top)]) + u"'";
    pattern = pattern.empty() ? temp : formatPattern(appendItemFor(g, top), pattern, temp, name);
    missing = t.distance.missing & ~(1u << Fraction);
  }
  return pattern;
}

// DateTimePatternGenerator::getBestPattern.
std::u16string getBestPattern(const Generator& g, const Skeleton& req) {
  unsigned all = req.mask();
  Best best = getBestRaw(g, req, all);
  if (best.match && best.distance.missing == 0 && best.distance.extra == 0)
    return patternString(adjustFieldTypes(parsePattern(best.match->pattern), req, &best.match->skeleton, all, g.decimal));
  std::u16string date = getBestAppending(g, req, all & kDateMask);
  std::u16string time = getBestAppending(g, req, all & kTimeMask);
  if (date.empty()) return time;
  if (time.empty()) return date;
  int style = 3;
  if (req.len[Month] == 4) style = req.ch[Weekday] ? 0 : 1;
  else if (req.len[Month] == 3) style = 2;
  return formatPattern(g.atTime[style], time, date);
}

// ---- time zones ----

struct TimeZone {
  std::string id;      // [[TimeZone]]: the identifier as given (case-regularized), or the offset string
  int offsetMinutes;   // offset zones
  bool isOffset;
  temporal::TimeZone named;  // named zones: offsets from Temporal's IANA data
};

// IsTimeZoneOffsetString: ±HH, ±HHMM, ±HH:MM (ECMA-402 accepts minute precision).
std::optional<int> parseOffset(std::string_view s) {
  if (s.size() < 3 || (s[0] != '+' && s[0] != '-')) return std::nullopt;
  int sign = s[0] == '-' ? -1 : 1;
  auto digits = [&](size_t at) -> int {
    if (at + 2 > s.size() || s[at] < '0' || s[at] > '9' || s[at + 1] < '0' || s[at + 1] > '9') return -1;
    return (s[at] - '0') * 10 + (s[at + 1] - '0');
  };
  int h = digits(1);
  if (h < 0 || h > 23) return std::nullopt;
  int m = 0;
  if (s.size() == 3) return sign * h * 60;
  if (s.size() == 5) m = digits(3);
  else if (s.size() == 6 && s[3] == ':') m = digits(4);
  else return std::nullopt;
  if (m < 0 || m > 59) return std::nullopt;
  return sign * (h * 60 + m);
}

std::string formatOffsetIdentifier(int minutes) {
  char buf[16];
  int a = std::abs(minutes);
  snprintf(buf, sizeof buf, "%c%02d:%02d", minutes < 0 ? '-' : '+', a / 60, a % 60);
  return buf;
}

// ---- the DateTimeFormat object ----

class DateTimeFormatObject : public Object {
 public:
  using Object::Object;
  std::string locale, dataLocale, calendar, numberingSystem;
  TimeZone timeZone;
  std::string timeZonePrimary;  // the primary identifier of a named zone
  std::string hourCycle;  // "" when the format has no hour
  std::optional<std::string> dateStyle, timeStyle;
  std::u16string pattern;  // [[DateTimeFormat]].[[pattern]]
  Skeleton requested;      // the requested skeleton (for range patterns)
  // The explicit component options (for the Temporal formats) and the hour cycle they use.
  std::vector<std::pair<std::u16string_view, std::string>> components;
  std::string formatHourCycle;
  Object* boundFormat = nullptr;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(boundFormat);
  }
};

constexpr const char* kRelevantKeys[] = {"ca", "hc", "nu"};

std::vector<std::string> dtfKeyValues(const std::string& locale, std::string_view key) {
  if (key == "ca") {
    std::vector<std::string> out{data::defaultCalendar(locale)};
    for (const std::string& c : data::calendars())
      if (c != out[0]) out.push_back(c);
    return out;
  }
  if (key == "hc") return {"", "h11", "h12", "h23", "h24"};
  if (key == "nu") {
    std::vector<std::string> out{data::defaultNumberingSystem(locale)};
    for (const std::string& nu : data::numberingSystems())
      if (nu != out[0]) out.push_back(nu);
    return out;
  }
  return {};
}
const ServiceData kDateTimeFormatData{data::locales, dtfKeyValues};

char16_t hourCharFor(const std::string& hc) { return hc == "h11" ? u'K' : hc == "h12" ? u'h' : hc == "h24" ? u'k' : u'H'; }
bool twelveHour(char16_t c) { return c == u'h' || c == u'K'; }

// Replace the hour fields of a pattern for an hour cycle, dropping the day period when going to 24 hours.
std::u16string applyHourCycle(const Generator& g, std::u16string_view pattern, const std::string& hc) {
  std::vector<Token> tokens = parsePattern(pattern);
  char16_t want = hourCharFor(hc);
  bool has = false, same = true;
  for (const Token& t : tokens) {
    if (t.field && fieldOf(t.ch) == Hour) {
      has = true;
      if (twelveHour(t.ch) != twelveHour(want)) same = false;
    }
  }
  if (!has) return std::u16string(pattern);
  if (!same) {
    // Regenerate from the pattern's skeleton with the wanted hour character.
    Skeleton k;
    for (const Token& t : tokens)
      if (t.field && t.ch != u'a' && t.ch != u'b') k.set(fieldOf(t.ch) == Hour ? want : t.ch, t.count);
    unsigned dateFields = k.mask() & kDateMask;
    if (dateFields) {
      // Keep the date part: only the time part is regenerated.
      Skeleton timeOnly;
      for (int f = 0; f < kFieldCount; f++)
        if ((kTimeMask & (1u << f)) && k.ch[f]) timeOnly.set(k.ch[f], k.len[f]);
      std::u16string time = getBestPattern(g, timeOnly);
      Skeleton dateOnly;
      for (int f = 0; f < kFieldCount; f++)
        if ((kDateMask & (1u << f)) && k.ch[f]) dateOnly.set(k.ch[f], k.len[f]);
      std::u16string date = getBestPattern(g, dateOnly);
      int style = 3;
      if (k.len[Month] == 4) style = k.ch[Weekday] ? 0 : 1;
      else if (k.len[Month] == 3) style = 2;
      return formatPattern(g.atTime[style], time, date);
    }
    return getBestPattern(g, k);
  }
  for (Token& t : tokens)
    if (t.field && fieldOf(t.ch) == Hour) t.ch = want;
  return patternString(tokens);
}

// ---- formatting ----

struct DatePart {
  const char16_t* type;
  std::u16string value;
  int source = 0;  // 0 shared, 1 startRange, 2 endRange
};

std::u16string digitsOf(const std::string& nu, int64_t value, int minDigits) {
  std::string s = std::to_string(value < 0 ? -value : value);
  if (int(s.size()) < minDigits) s = std::string(size_t(minDigits) - s.size(), '0') + s;
  if (value < 0) s = "-" + s;
  std::u16string out;
  const char16_t* digits = nu == "latn" ? nullptr : data::numberingSystemDigits(nu);
  for (char c : s) {
    if (c >= '0' && c <= '9' && digits) {
      const char16_t* p = digits;
      for (int i = 0; i < c - '0'; i++) p += (p[0] >= 0xD800 && p[0] <= 0xDBFF) ? 2 : 1;
      out.push_back(p[0]);
      if (p[0] >= 0xD800 && p[0] <= 0xDBFF) out.push_back(p[1]);
    } else {
      out.push_back(char16_t(c));
    }
  }
  return out;
}

// The UTC offset (ms) of the format's time zone at an instant.
int64_t offsetMsAt(const DateTimeFormatObject& dtf, double epochMs) {
  if (dtf.timeZone.isOffset) return int64_t(dtf.timeZone.offsetMinutes) * 60000;
  temporal::EpochNanoseconds ns = temporal::EpochNanoseconds(int64_t(epochMs)) * 1000000;
  if (!temporal::IsValidEpochNanoseconds(ns)) return 0;
  return int64_t(temporal::GetOffsetNanosecondsFor(dtf.timeZone.named, ns) / 1000000);
}

// Whether daylight saving time is in effect: the offset exceeds the year's smaller one (January vs July).
bool isDaylight(const DateTimeFormatObject& dtf, double epochMs) {
  if (dtf.timeZone.isOffset) return false;
  LocalTime t = toLocalTime(epochMs);
  auto at = [&](int month) {
    double ms = double((t.year - 1970) * 365.2425 * 86400000.0) + month * 30.4 * 86400000.0;
    return offsetMsAt(dtf, ms);
  };
  return offsetMsAt(dtf, epochMs) > std::min(at(0), at(6));
}

// The localized GMT format: short "GMT+5:30" / "GMT-8", long "GMT+05:30"; a zero offset is the gmtZeroFormat ("GMT").
std::u16string gmtString(const DateTimeFormatObject& dtf, int offsetMinutes, bool longForm) {
  std::vector<std::u16string_view> f = items(dateField(dtf.dataLocale, FZoneFormats));
  f.resize(3);
  if (offsetMinutes == 0 && !f[2].empty()) return std::u16string(f[2]);
  std::u16string_view hourFormat = f[0];
  size_t semi = hourFormat.find(u';');
  std::u16string_view pat = offsetMinutes >= 0 ? hourFormat.substr(0, semi) : hourFormat.substr(semi + 1);
  int a = std::abs(offsetMinutes), h = a / 60, m = a % 60;
  std::u16string out;
  std::vector<Token> tokens = parsePattern(pat);
  for (size_t i = 0; i < tokens.size(); i++) {
    const Token& t = tokens[i];
    bool beforeMinutes = i + 1 < tokens.size() && tokens[i + 1].field && tokens[i + 1].ch == u'm';
    if (!t.field) {
      if (!longForm && m == 0 && beforeMinutes) continue;
      out += t.text;
    } else if (t.ch == u'H') {
      out += digitsOf(dtf.numberingSystem, h, longForm ? 2 : 1);
    } else if (t.ch == u'm') {
      if (longForm || m != 0) out += digitsOf(dtf.numberingSystem, m, 2);
    }
  }
  return formatPattern(f[1], out, u"");
}

std::u16string zoneName(const DateTimeFormatObject& dtf, char16_t c, int count, double epochMs, int offsetMinutes) {
  const TimeZone& tz = dtf.timeZone;
  bool longForm = count >= 4;
  if (c == u'O' || tz.isOffset) return gmtString(dtf, offsetMinutes, longForm);
  if (tz.named.isUTC()) {
    if (c == u'z') {
      std::vector<std::u16string_view> n = items(dateField(dtf.dataLocale, FUtcNames));
      n.resize(2);
      std::u16string_view name = longForm ? n[0] : n[1];
      if (!name.empty()) return std::u16string(name);
    }
    return gmtString(dtf, 0, longForm);
  }
  // A named zone: the English metazone name (generic, or standard/daylight), else the GMT format.
  std::string language = dtf.dataLocale.substr(0, dtf.dataLocale.find('-'));
  if (language == "en" && (c == u'z' || c == u'v')) {
    for (const std::string& id : {tz.id, dtf.timeZonePrimary}) {
      const AliasEntry* end = std::end(kZoneMetazones);
      const AliasEntry* it = std::lower_bound(std::begin(kZoneMetazones), end, id,
                                              [](const AliasEntry& e, const std::string& k) { return std::string_view(e.from) < k; });
      if (it == end || id != it->from) continue;
      if (const char16_t* names = findEntry(kMetazoneNames, it->to)) {
        std::vector<std::u16string_view> n = items(names);
        n.resize(6);
        int index = (longForm ? 0 : 3) + (c == u'v' ? 0 : isDaylight(dtf, epochMs) ? 2 : 1);
        if (!n[size_t(index)].empty()) return std::u16string(n[size_t(index)]);
      }
      break;
    }
  }
  return gmtString(dtf, offsetMinutes, longForm);
}

// The flexible day period of a time (CLDR dayPeriodRules; midnight is not used, as in ICU).
std::u16string flexibleDayPeriod(const DateTimeFormatObject& dtf, const LocalTime& t, int width, bool hasMinute, bool hasSecond) {
  std::vector<std::u16string_view> names = items(dateField(dtf.dataLocale, FDayPeriodsAbbr + (width == 5 ? 1 : width == 4 ? 2 : 0)));
  names.resize(12);
  static const char16_t* const keys[] = {u"am", u"pm", u"midnight", u"noon", u"morning1", u"morning2", u"afternoon1", u"afternoon2",
                                         u"evening1", u"evening2", u"night1", u"night2"};
  auto nameOf = [&](std::u16string_view key) -> std::u16string_view {
    for (int i = 0; i < 12; i++)
      if (key == keys[i]) return names[size_t(i)];
    return {};
  };
  std::vector<std::u16string_view> rules = items(dateField(dtf.dataLocale, FDayPeriodRules));
  int minute = t.hour * 60;
  bool exact = (!hasMinute || t.minute == 0) && (!hasSecond || t.second == 0);
  if (exact && t.hour == 12 && !nameOf(u"noon").empty()) {
    for (std::u16string_view r : rules)
      if (r == u"noon=@720") return std::u16string(nameOf(u"noon"));
  }
  for (std::u16string_view r : rules) {
    size_t eq = r.find(u'=');
    std::u16string_view key = r.substr(0, eq), value = r.substr(eq + 1);
    if (value[0] == u'@') continue;
    size_t dash = value.find(u'-');
    int from = 0, before = 0;
    for (char16_t c : value.substr(0, dash)) from = from * 10 + (c - u'0');
    for (char16_t c : value.substr(dash + 1)) before = before * 10 + (c - u'0');
    bool in = from < before ? (minute >= from && minute < before) : (minute >= from || minute < before);
    if (in && !nameOf(key).empty()) return std::u16string(nameOf(key));
  }
  return std::u16string(t.hour < 12 ? names[0] : names[1]);
}

// Formats `pattern` at an instant; plain Temporal values (utc) are formatted at offset zero with no zone.
void formatPatternInto(std::vector<DatePart>& out, const DateTimeFormatObject& dtf, std::u16string_view pattern, double epochMs, int source,
                       bool utc = false) {
  int64_t offset = utc ? 0 : offsetMsAt(dtf, epochMs);
  double local = epochMs + double(offset);
  LocalTime t = toLocalTime(local);
  std::vector<Token> tokens = parsePattern(pattern);
  bool hasMinute = false, hasSecond = false;
  for (const Token& tok : tokens) {
    hasMinute |= tok.field && tok.ch == u'm';
    hasSecond |= tok.field && tok.ch == u's';
  }
  const std::string& nu = dtf.numberingSystem;
  for (size_t ti = 0; ti < tokens.size(); ti++) {
    const Token& tok = tokens[ti];
    if (!tok.field) {
      // The decimal separator before fractional seconds is the numbering system's.
      std::u16string text = tok.text;
      if (text == u"." && ti + 1 < tokens.size() && tokens[ti + 1].field && tokens[ti + 1].ch == u'S')
        text = decimalSeparator(dtf.dataLocale, dtf.numberingSystem);
      if (!out.empty() && std::u16string_view(out.back().type) == u"literal" && out.back().source == source) out.back().value += text;
      else out.push_back({u"literal", text, source});
      continue;
    }
    int n = tok.count;
    switch (tok.ch) {
      case u'G': {
        int index = t.year > 0 ? 1 : 0;
        int field = FEraAbbr + (n == 4 ? 1 : n == 5 ? 2 : 0);
        std::vector<std::u16string_view> eras = items(dateField(dtf.dataLocale, field));
        eras.resize(2);
        out.push_back({u"era", std::u16string(eras[size_t(index)]), source});
        break;
      }
      case u'y': case u'Y': case u'u': case u'r': {
        int64_t year = tok.ch == u'y' || tok.ch == u'Y' ? (t.year > 0 ? t.year : 1 - t.year) : t.year;
        if (n == 2) out.push_back({u"year", digitsOf(nu, ((year % 100) + 100) % 100, 2), source});
        else out.push_back({u"year", digitsOf(nu, year, n), source});
        break;
      }
      case u'M': case u'L': {
        if (n <= 2) {
          out.push_back({u"month", digitsOf(nu, t.month, n), source});
        } else {
          int base = tok.ch == u'L' ? 3 : 0;
          int field = FMonthsFormatAbbr + base + (n == 5 ? 1 : n == 4 ? 2 : 0);
          std::vector<std::u16string_view> names = items(dateField(dtf.dataLocale, field));
          names.resize(12);
          out.push_back({u"month", std::u16string(names[size_t(t.month - 1)]), source});
        }
        break;
      }
      case u'd':
        out.push_back({u"day", digitsOf(nu, t.day, n), source});
        break;
      case u'E': case u'c': case u'e': {
        if (tok.ch != u'E' && n <= 2) {
          out.push_back({u"weekday", digitsOf(nu, t.weekday + 1, n), source});
          break;
        }
        int base = tok.ch == u'c' ? 4 : 0;
        int width = n == 4 ? 3 : n == 5 ? 1 : n == 6 ? 2 : 0;
        std::vector<std::u16string_view> names = items(dateField(dtf.dataLocale, FDaysFormatAbbr + base + width));
        names.resize(7);
        out.push_back({u"weekday", std::u16string(names[size_t(t.weekday)]), source});
        break;
      }
      case u'a': case u'b': {
        std::vector<std::u16string_view> names = items(dateField(dtf.dataLocale, FDayPeriodsAbbr + (n == 5 ? 1 : n == 4 ? 2 : 0)));
        names.resize(4);
        std::u16string_view name = t.hour < 12 ? names[0] : names[1];
        if (tok.ch == u'b' && t.hour == 12 && t.minute == 0 && t.second == 0 && !names[3].empty()) name = names[3];
        out.push_back({u"dayPeriod", std::u16string(name), source});
        break;
      }
      case u'B':
        out.push_back({u"dayPeriod", flexibleDayPeriod(dtf, t, n, hasMinute, hasSecond), source});
        break;
      case u'h': out.push_back({u"hour", digitsOf(nu, t.hour % 12 == 0 ? 12 : t.hour % 12, n), source}); break;
      case u'H': out.push_back({u"hour", digitsOf(nu, t.hour, n), source}); break;
      case u'K': out.push_back({u"hour", digitsOf(nu, t.hour % 12, n), source}); break;
      case u'k': out.push_back({u"hour", digitsOf(nu, t.hour == 0 ? 24 : t.hour, n), source}); break;
      case u'm': out.push_back({u"minute", digitsOf(nu, t.minute, n), source}); break;
      case u's': out.push_back({u"second", digitsOf(nu, t.second, n), source}); break;
      case u'S': {
        std::string ms = std::to_string(1000 + t.millisecond).substr(1) + std::string(size_t(std::max(0, n - 3)), '0');
        std::u16string v;
        for (char c : ms.substr(0, size_t(n))) v += digitsOf(nu, c - '0', 1);
        out.push_back({u"fractionalSecond", v, source});
        break;
      }
      case u'z': case u'Z': case u'O': case u'v': case u'V': case u'X': case u'x':
        out.push_back({u"timeZoneName", zoneName(dtf, tok.ch, n, epochMs, int(offset / 60000)), source});
        break;
      default:
        break;
    }
  }
}

// ---- options ----

struct ComponentOption {
  const char16_t* name;
  std::initializer_list<std::string_view> values;
};

// Table 16: the date and time components, in order.
const ComponentOption kComponents[] = {
    {u"weekday", {"narrow", "short", "long"}},
    {u"era", {"narrow", "short", "long"}},
    {u"year", {"2-digit", "numeric"}},
    {u"month", {"2-digit", "numeric", "narrow", "short", "long"}},
    {u"day", {"2-digit", "numeric"}},
    {u"dayPeriod", {"narrow", "short", "long"}},
    {u"hour", {"2-digit", "numeric"}},
    {u"minute", {"2-digit", "numeric"}},
    {u"second", {"2-digit", "numeric"}},
    {u"fractionalSecondDigits", {}},
    {u"timeZoneName", {"short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric"}},
};

void skeletonAdd(Skeleton& k, std::u16string_view name, const std::string& value, char16_t hourChar) {
  auto width = [&](int shortLen) { return value == "narrow" ? 5 : value == "long" ? 4 : shortLen; };
  if (name == u"weekday") k.set(u'E', width(3));
  else if (name == u"era") k.set(u'G', value == "narrow" ? 5 : value == "long" ? 4 : 1);
  else if (name == u"year") k.set(u'y', value == "2-digit" ? 2 : 1);
  else if (name == u"month") k.set(u'M', value == "2-digit" ? 2 : value == "numeric" ? 1 : width(3));
  else if (name == u"day") k.set(u'd', value == "2-digit" ? 2 : 1);
  else if (name == u"dayPeriod") k.set(u'B', value == "narrow" ? 5 : value == "long" ? 4 : 1);
  else if (name == u"hour") k.set(hourChar, value == "2-digit" ? 2 : 1);
  else if (name == u"minute") k.set(u'm', value == "2-digit" ? 2 : 1);
  else if (name == u"second") k.set(u's', value == "2-digit" ? 2 : 1);
  else if (name == u"fractionalSecondDigits") k.set(u'S', value[0] - '0');
  else if (name == u"timeZoneName") {
    if (value == "short") k.set(u'z', 1);
    else if (value == "long") k.set(u'z', 4);
    else if (value == "shortOffset") k.set(u'O', 1);
    else if (value == "longOffset") k.set(u'O', 4);
    else if (value == "shortGeneric") k.set(u'v', 1);
    else k.set(u'v', 4);
  }
}

}  // namespace

enum class DateRequired2 : uint8_t { Date, Time, Any };
enum class DateDefaults : uint8_t { Date, Time, All };

namespace {

int styleIndex(const std::string& s) { return s == "full" ? 0 : s == "long" ? 1 : s == "medium" ? 2 : 3; }

// 11.1.2 CreateDateTimeFormat ( newTarget, locales, options, required, defaults [ , toLocaleStringTimeZone ] )
Result<DateTimeFormatObject*> CreateDateTimeFormat(Object* newTarget, Value locales, Value optionsValue, DateRequired2 required,
                                                   DateDefaults defaults,
                                                   const temporal::TimeZone* toLocaleStringTimeZone = nullptr) {
  DateTimeFormatObject* dtf = TRY(OrdinaryCreateFromConstructor<DateTimeFormatObject>(newTarget, Intrinsic::IntlDateTimeFormatPrototype));
  std::vector<std::string> requested = TRY(CanonicalizeLocaleList(locales));
  Object* options = TRY(CoerceOptionsToObject(optionsValue));
  LocaleOptions opt;
  opt.localeMatcher = TRY(GetLocaleMatcher(options));
  std::optional<std::string> calendar = TRY(GetStringOption(options, u"calendar", {}));
  if (calendar && !isUnicodeLocaleType(*calendar)) return ThrowRangeError("Intl.DateTimeFormat: invalid calendar " + *calendar);
  if (calendar) opt.keys.emplace_back("ca", LocaleOption::string(*calendar));
  std::optional<std::string> numberingSystem = TRY(GetStringOption(options, u"numberingSystem", {}));
  if (numberingSystem && !isUnicodeLocaleType(*numberingSystem))
    return ThrowRangeError("Intl.DateTimeFormat: invalid numberingSystem " + *numberingSystem);
  if (numberingSystem) opt.keys.emplace_back("nu", LocaleOption::string(*numberingSystem));
  std::optional<bool> hour12 = TRY(GetBooleanOption(options, u"hour12"));
  std::optional<std::string> hourCycle = TRY(GetStringOption(options, u"hourCycle", {"h11", "h12", "h23", "h24"}));
  if (hour12) opt.keys.emplace_back("hc", LocaleOption::null());
  else if (hourCycle) opt.keys.emplace_back("hc", LocaleOption::string(*hourCycle));
  ResolvedLocale r = ResolveLocale(kDateTimeFormatData, requested, opt, kRelevantKeys);
  dtf->locale = r.locale;
  dtf->dataLocale = r.dataLocale;
  dtf->calendar = r.get("ca");
  dtf->numberingSystem = r.get("nu");
  std::string defaultHc = data::defaultHourCycle(r.dataLocale);
  std::string language = r.dataLocale.substr(0, r.dataLocale.find('-'));
  std::string hc;
  if (hour12) hc = *hour12 ? (defaultHc == "h11" || language == "ja" ? "h11" : "h12") : (defaultHc == "h24" ? "h24" : "h23");
  else hc = r.get("hc").empty() ? defaultHc : r.get("hc");
  if (hour12 && *hour12 && language == "ja") hc = "h11";

  Value timeZoneValue = TRY(Get(options, PropertyKey(u"timeZone")));
  std::string timeZone;
  if (toLocaleStringTimeZone) {
    // Temporal.ZonedDateTime.prototype.toLocaleString: the ZonedDateTime's own time zone, never an option.
    if (!timeZoneValue.isUndefined())
      return ThrowTypeError("Temporal.ZonedDateTime.prototype.toLocaleString: the timeZone option is not allowed");
    timeZone = toLocaleStringTimeZone->isOffset() ? formatOffsetIdentifier(toLocaleStringTimeZone->offsetMinutes)
                                                  : utf16ToUtf8(temporal::TimeZoneIdentifier(*toLocaleStringTimeZone));
  } else if (timeZoneValue.isUndefined()) {
    timeZone = "UTC";  // SystemTimeZoneIdentifier: the host time zone of Date (TZ unset: UTC)
  } else {
    String* s = TRY(ToString(timeZoneValue));
    std::optional<std::string> ascii = toAscii(s->view());
    if (!ascii) return ThrowRangeError("Intl.DateTimeFormat: invalid time zone " + s->toUtf8());
    timeZone = *ascii;
  }
  if (std::optional<int> offset = parseOffset(timeZone)) {
    dtf->timeZone = {formatOffsetIdentifier(*offset), *offset, true, temporal::TimeZone::Offset(*offset)};
  } else if (std::optional<temporal::TimeZone> named = temporal::GetAvailableNamedTimeZoneIdentifier(builtins::toU16(timeZone))) {
    // [[TimeZone]] keeps the identifier as given (case-regularized), not its primary identifier.
    dtf->timeZone = {utf16ToUtf8(temporal::TimeZoneIdentifier(*named)), 0, false, *named};
    dtf->timeZonePrimary = utf16ToUtf8(temporal::PrimaryTimeZoneIdentifier(*named));
  } else {
    return ThrowRangeError("Intl.DateTimeFormat: invalid time zone " + timeZone);
  }

  bool hasExplicitFormatComponents = false;
  dtf->formatHourCycle = hc;
  std::vector<std::pair<std::u16string_view, std::string>> formatOptions;
  for (const ComponentOption& c : kComponents) {
    std::optional<std::string> value;
    if (std::u16string_view(c.name) == u"fractionalSecondDigits") {
      std::optional<double> fsd = TRY(GetNumberOption(options, c.name, 1, 3, std::nullopt));
      if (fsd) value = std::to_string(int(*fsd));
    } else {
      value = TRY(GetStringOption(options, c.name, c.values));
    }
    if (value) {
      formatOptions.emplace_back(c.name, *value);
      hasExplicitFormatComponents = true;
    }
  }
  dtf->components = formatOptions;
  TRY(GetStringOption(options, u"formatMatcher", {"basic", "best fit"}, "best fit"));
  dtf->dateStyle = TRY(GetStringOption(options, u"dateStyle", {"full", "long", "medium", "short"}));
  dtf->timeStyle = TRY(GetStringOption(options, u"timeStyle", {"full", "long", "medium", "short"}));
  const Generator& g = generatorFor(r.dataLocale);
  std::u16string pattern;
  if (dtf->dateStyle || dtf->timeStyle) {
    if (hasExplicitFormatComponents) return ThrowTypeError("Intl.DateTimeFormat: dateStyle/timeStyle cannot be combined with components");
    if (required == DateRequired2::Date && dtf->timeStyle) return ThrowTypeError("Intl.DateTimeFormat: timeStyle is not allowed here");
    if (required == DateRequired2::Time && dtf->dateStyle) return ThrowTypeError("Intl.DateTimeFormat: dateStyle is not allowed here");
    std::u16string date, time;
    if (dtf->dateStyle) date = std::u16string(items(dateField(r.dataLocale, FDateFormats))[size_t(styleIndex(*dtf->dateStyle))]);
    if (dtf->timeStyle) time = applyHourCycle(g, items(dateField(r.dataLocale, FTimeFormats))[size_t(styleIndex(*dtf->timeStyle))], hc);
    if (!date.empty() && !time.empty()) pattern = formatPattern(g.atTime[styleIndex(*dtf->dateStyle)], time, date);
    else pattern = date.empty() ? time : date;
  } else {
    bool needDefaults = true;
    auto has = [&](std::u16string_view name) {
      for (const auto& [n, v] : formatOptions)
        if (n == name) return true;
      return false;
    };
    if (required != DateRequired2::Time)
      for (const char16_t* p : {u"weekday", u"year", u"month", u"day"}) needDefaults &= !has(p);
    if (required != DateRequired2::Date)
      for (const char16_t* p : {u"dayPeriod", u"hour", u"minute", u"second", u"fractionalSecondDigits"}) needDefaults &= !has(p);
    if (needDefaults && defaults != DateDefaults::Time)
      for (const char16_t* p : {u"year", u"month", u"day"}) formatOptions.emplace_back(p, "numeric");
    if (needDefaults && defaults != DateDefaults::Date)
      for (const char16_t* p : {u"hour", u"minute", u"second"}) formatOptions.emplace_back(p, "numeric");
    // A ZonedDateTime's default format names its time zone.
    if (needDefaults && toLocaleStringTimeZone && !has(u"timeZoneName")) formatOptions.emplace_back(u"timeZoneName", "short");
    Skeleton k;
    for (const auto& [name, value] : formatOptions) skeletonAdd(k, name, value, hourCharFor(hc));
    k.normalizeDayPeriod();
    dtf->requested = k;
    pattern = getBestPattern(g, k);
    pattern = applyHourCycle(g, pattern, hc);
  }
  dtf->pattern = pattern;
  bool hasHour = false;
  for (const Token& t : parsePattern(pattern)) hasHour |= t.field && fieldOf(t.ch) == Hour;
  dtf->hourCycle = hasHour ? hc : "";
  return dtf;
}

Result<Value> DateTimeFormat_construct(NativeCall& call) {
  Object* newTarget = builtins::newTargetOrCallee(call);
  DateTimeFormatObject* dtf = TRY(CreateDateTimeFormat(newTarget, call.args[0], call.args[1], DateRequired2::Any, DateDefaults::Date));
  // ChainDateTimeFormat ( dateTimeFormat, newTarget, this )
  if (!call.newTarget && call.thisValue.isObject()) {
    bool instance = TRY(OrdinaryHasInstance(builtins::intrinsic(Intrinsic::IntlDateTimeFormat), call.thisValue));
    if (instance) {
      TRY(DefinePropertyOrThrow(call.thisValue.asObject(), PropertyKey(fallbackSymbol()), PropertyDescriptor::data(Value(dtf), Attr::None)));
      return call.thisValue;
    }
  }
  return Value(dtf);
}

Result<Value> DateTimeFormat_supportedLocalesOf(NativeCall& call) { return supportedLocalesOf(kDateTimeFormatData, call); }

// UnwrapDateTimeFormat ( dtf )
Result<DateTimeFormatObject*> unwrapDateTimeFormat(Value v, const char* method) {
  if (!v.isObject()) return ThrowTypeError(std::string(method) + " called on incompatible receiver");
  Object* o = v.asObject();
  if (!o->is<DateTimeFormatObject>()) {
    bool instance = TRY(OrdinaryHasInstance(builtins::intrinsic(Intrinsic::IntlDateTimeFormat), v));
    if (instance) {
      Value inner = TRY(Get(o, PropertyKey(fallbackSymbol())));
      if (inner.isObject() && inner.asObject()->is<DateTimeFormatObject>()) return inner.asObject()->as<DateTimeFormatObject>();
    }
    return ThrowTypeError(std::string(method) + " called on incompatible receiver");
  }
  return o->as<DateTimeFormatObject>();
}

double timeClip(double t) {
  if (!std::isfinite(t) || std::fabs(t) > 8.64e15) return NAN;
  return std::trunc(t) + 0.0;
}

// ---- Temporal values (ToDateTimeFormattable, HandleDateTimeValue) ----

enum class ValueKind : uint8_t { Number, PlainDate, PlainYearMonth, PlainMonthDay, PlainTime, PlainDateTime, Instant, ZonedDateTime };

struct Formattable {
  ValueKind kind = ValueKind::Number;
  double epochMs = 0;          // the instant (plain values: their ISO date-time read as UTC)
  std::u16string calendar;     // plain dates
};

double isoEpochMs(const temporal::ISODate& d, const temporal::TimeRecord& t) {
  return double(daysFromCivil(d.year, d.month, d.day)) * 86400000.0 + t.hour * 3600000.0 + t.minute * 60000.0 + t.second * 1000.0 +
         t.millisecond;
}

// ToDateTimeFormattable ( value )
Result<Formattable> toFormattable(Value v) {
  Formattable f;
  temporal::TimeRecord noon;
  noon.hour = 12;
  if (auto* d = temporal::asTemporal<temporal::PlainDateObject>(v)) return Formattable{ValueKind::PlainDate, isoEpochMs(d->isoDate, noon), d->calendar};
  if (auto* d = temporal::asTemporal<temporal::PlainYearMonthObject>(v))
    return Formattable{ValueKind::PlainYearMonth, isoEpochMs(d->isoDate, noon), d->calendar};
  if (auto* d = temporal::asTemporal<temporal::PlainMonthDayObject>(v))
    return Formattable{ValueKind::PlainMonthDay, isoEpochMs(d->isoDate, noon), d->calendar};
  if (auto* d = temporal::asTemporal<temporal::PlainTimeObject>(v)) return Formattable{ValueKind::PlainTime, isoEpochMs({1970, 1, 1}, d->time), {}};
  if (auto* d = temporal::asTemporal<temporal::PlainDateTimeObject>(v))
    return Formattable{ValueKind::PlainDateTime, isoEpochMs(d->isoDateTime.isoDate, d->isoDateTime.time), d->calendar};
  if (auto* d = temporal::asTemporal<temporal::InstantObject>(v)) {
    temporal::EpochNanoseconds ns = d->epochNanoseconds;
    temporal::EpochNanoseconds ms = ns / 1000000 - (ns % 1000000 < 0 ? 1 : 0);
    return Formattable{ValueKind::Instant, double(int64_t(ms)), {}};
  }
  if (temporal::asTemporal<temporal::ZonedDateTimeObject>(v)) return Formattable{ValueKind::ZonedDateTime, 0, {}};
  f.epochMs = TRY(ToNumber(v));
  return f;
}

// Drops the time zone fields (and the space before them) from a pattern.
std::u16string withoutZone(std::u16string_view pattern) {
  std::vector<Token> tokens = parsePattern(pattern), out;
  for (const Token& t : tokens) {
    if (t.field && fieldOf(t.ch) == Zone) {
      if (!out.empty() && !out.back().field) {
        std::u16string& text = out.back().text;
        while (!text.empty() && (text.back() == u' ' || text.back() == u'\u00a0' || text.back() == u'\u202f')) text.pop_back();
        if (text.empty()) out.pop_back();
      }
      continue;
    }
    out.push_back(t);
  }
  return patternString(out);
}

// The pattern of a format restricted to some fields, regenerated (PlainYearMonth / PlainMonthDay date styles).
std::u16string restrictedPattern(const Generator& g, std::u16string_view pattern, unsigned fields) {
  Skeleton k;
  for (const Token& t : parsePattern(pattern))
    if (t.field && fieldOf(t.ch) >= 0 && (fields & (1u << fieldOf(t.ch)))) k.set(t.ch, t.count);
  return getBestPattern(g, k);
}

// The [[TemporalPlain*Format]] of a kind (GetDateTimeFormat with inherit ~relevant~), "" for null. toLocaleString
// formats with the format its own CreateDateTimeFormat made for the type, so there options the type has no field
// for (era on a PlainTime) are dropped instead of nulling the format.
std::u16string temporalPattern(const DateTimeFormatObject& dtf, ValueKind kind, bool toLocaleString) {
  const Generator& g = generatorFor(dtf.dataLocale);
  if (dtf.dateStyle || dtf.timeStyle) {
    std::u16string date, time;
    if (dtf.dateStyle) date = std::u16string(items(dateField(dtf.dataLocale, FDateFormats))[size_t(styleIndex(*dtf.dateStyle))]);
    if (dtf.timeStyle) time = withoutZone(applyHourCycle(g, items(dateField(dtf.dataLocale, FTimeFormats))[size_t(styleIndex(*dtf.timeStyle))], dtf.formatHourCycle));
    switch (kind) {
      case ValueKind::PlainDate: return date;
      case ValueKind::PlainYearMonth: return date.empty() ? date : restrictedPattern(g, date, (1u << Era) | (1u << Year) | (1u << Month));
      case ValueKind::PlainMonthDay: return date.empty() ? date : restrictedPattern(g, date, (1u << Month) | (1u << Day));
      case ValueKind::PlainTime: return time;
      default:
        if (!date.empty() && !time.empty()) return formatPattern(g.atTime[styleIndex(*dtf.dateStyle)], time, date);
        return date.empty() ? time : date;
    }
  }
  std::vector<std::u16string_view> relevant, required, defaults;
  switch (kind) {
    case ValueKind::PlainDate:
      relevant = {u"weekday", u"era", u"year", u"month", u"day"};
      required = {u"weekday", u"year", u"month", u"day"};
      defaults = {u"year", u"month", u"day"};
      break;
    case ValueKind::PlainYearMonth:
      relevant = {u"era", u"year", u"month"};
      required = {u"year", u"month"};
      defaults = required;
      break;
    case ValueKind::PlainMonthDay:
      relevant = {u"month", u"day"};
      required = relevant;
      defaults = relevant;
      break;
    case ValueKind::PlainTime:
      relevant = {u"dayPeriod", u"hour", u"minute", u"second", u"fractionalSecondDigits"};
      required = relevant;
      defaults = {u"hour", u"minute", u"second"};
      break;
    default:
      relevant = {u"weekday", u"era", u"year", u"month", u"day", u"dayPeriod", u"hour", u"minute", u"second", u"fractionalSecondDigits"};
      required = {u"weekday", u"year", u"month", u"day", u"dayPeriod", u"hour", u"minute", u"second", u"fractionalSecondDigits"};
      defaults = {u"year", u"month", u"day", u"hour", u"minute", u"second"};
      break;
  }
  auto contains = [](const std::vector<std::u16string_view>& list, std::u16string_view name) {
    return std::find(list.begin(), list.end(), name) != list.end();
  };
  std::vector<std::pair<std::u16string_view, std::string>> options;
  // Null when components were given but none of them applies to this kind (year for a PlainMonthDay).
  bool anyPresent = false, anyRelevant = false, needDefaults = true;
  for (const auto& [name, value] : dtf.components) {
    if (name != u"timeZoneName") anyPresent = true;
    if (contains(relevant, name)) options.emplace_back(name, value), anyRelevant = true;
    if (contains(required, name)) needDefaults = false;
  }
  if (needDefaults) {
    if (anyPresent && !anyRelevant && !toLocaleString) return {};
    for (std::u16string_view d : defaults) options.emplace_back(d, "numeric");
  }
  Skeleton k;
  for (const auto& [name, value] : options) skeletonAdd(k, name, value, hourCharFor(dtf.formatHourCycle));
  k.normalizeDayPeriod();
  return applyHourCycle(g, getBestPattern(g, k), dtf.formatHourCycle);
}

// [[TemporalInstantFormat]]: GetDateTimeFormat(formats, matcher, options, ~any~, ~all~, ~all~). The format's own
// pattern, unless no date or time component was given: then the defaults are the date and the time, whatever the
// constructor's defaults were (Intl.DateTimeFormat() formats an Instant with its time).
std::u16string instantPattern(const DateTimeFormatObject& dtf) {
  if (dtf.dateStyle || dtf.timeStyle) return dtf.pattern;
  for (const auto& [name, value] : dtf.components)
    for (std::u16string_view c : {u"weekday", u"year", u"month", u"day", u"dayPeriod", u"hour", u"minute", u"second", u"fractionalSecondDigits"})
      if (name == c) return dtf.pattern;
  const Generator& g = generatorFor(dtf.dataLocale);
  Skeleton k;
  for (const auto& [name, value] : dtf.components) skeletonAdd(k, name, value, hourCharFor(dtf.formatHourCycle));
  for (const char16_t* p : {u"year", u"month", u"day", u"hour", u"minute", u"second"}) skeletonAdd(k, p, "numeric", hourCharFor(dtf.formatHourCycle));
  k.normalizeDayPeriod();
  return applyHourCycle(g, getBestPattern(g, k), dtf.formatHourCycle);
}

// HandleDateTimeValue: the pattern a value is formatted with, and whether it is a plain (UTC) value.
Result<std::pair<std::u16string, bool>> patternForValue(const DateTimeFormatObject& dtf, const Formattable& v,
                                                         bool toLocaleString = false) {
  switch (v.kind) {
    case ValueKind::Number: {
      if (!std::isfinite(v.epochMs) || std::fabs(v.epochMs) > 8.64e15) return ThrowRangeError("Invalid time value");
      return std::make_pair(dtf.pattern, false);
    }
    case ValueKind::ZonedDateTime:
      return ThrowTypeError("Intl.DateTimeFormat: Temporal.ZonedDateTime is not supported; use its toLocaleString");
    case ValueKind::Instant:
      return std::make_pair(instantPattern(dtf), false);
    default: {
      std::string calendar = utf16ToUtf8(v.calendar);
      bool yearMonthOrMonthDay = v.kind == ValueKind::PlainYearMonth || v.kind == ValueKind::PlainMonthDay;
      if (v.kind != ValueKind::PlainTime && calendar != dtf.calendar && (yearMonthOrMonthDay || calendar != "iso8601"))
        return ThrowRangeError("Intl.DateTimeFormat: the calendar " + calendar + " is not the format's " + dtf.calendar);
      std::u16string pattern = temporalPattern(dtf, v.kind, toLocaleString);
      if (pattern.empty()) return ThrowTypeError("Intl.DateTimeFormat: the options have no field this Temporal value has");
      return std::make_pair(pattern, true);
    }
  }
}

Result<std::vector<DatePart>> formatParts(const DateTimeFormatObject& dtf, const Formattable& x, bool toLocaleString = false) {
  auto [pattern, utc] = TRY(patternForValue(dtf, x, toLocaleString));
  std::vector<DatePart> parts;
  formatPatternInto(parts, dtf, pattern, x.kind == ValueKind::Number ? timeClip(x.epochMs) : x.epochMs, 0, utc);
  return parts;
}

Result<std::vector<DatePart>> formatParts(const DateTimeFormatObject& dtf, double x) {
  Formattable f;
  f.epochMs = x;
  return formatParts(dtf, f);
}

// The date argument of format/formatToParts: undefined is now, else ToDateTimeFormattable.
Result<Formattable> formattableArgument(Value date) {
  if (date.isUndefined()) {
    Formattable f;
    Value now = TRY(Invoke(Value(builtins::intrinsic(Intrinsic::Date)), PropertyKey(u"now")));
    f.epochMs = now.asNumber();
    return f;
  }
  return toFormattable(date);
}

std::u16string joinParts(const std::vector<DatePart>& parts) {
  std::u16string out;
  for (const DatePart& p : parts) out += p.value;
  return out;
}

Result<Value> boundFormat(NativeCall& call) {
  DateTimeFormatObject* dtf = call.callee->slots[0].asObject()->as<DateTimeFormatObject>();
  Formattable x = TRY(formattableArgument(call.args[0]));
  std::vector<DatePart> parts = TRY(formatParts(*dtf, x));
  return Value(newString(joinParts(parts)));
}

Result<Value> DateTimeFormatPrototype_format(NativeCall& call) {
  DateTimeFormatObject* dtf = TRY(unwrapDateTimeFormat(call.thisValue, "get Intl.DateTimeFormat.prototype.format"));
  if (!dtf->boundFormat) {
    BuiltinFunction* f = createBuiltinFunction(*builtins::currentRealm(), boundFormat, 1, PropertyKey(u""));
    f->slots.push_back(Value(dtf));
    dtf->boundFormat = f;
  }
  return Value(dtf->boundFormat);
}

Object* partsArray(const std::vector<DatePart>& parts, bool withSource) {
  ValueVector values;
  static const char16_t* const sources[] = {u"shared", u"startRange", u"endRange"};
  for (const DatePart& p : parts) {
    Object* o = builtins::newPlainObject();
    addProperty(o, u"type", str(p.type));
    addProperty(o, u"value", Value(newString(p.value)));
    if (withSource) addProperty(o, u"source", str(sources[p.source]));
    values.push_back(Value(o));
  }
  return CreateArrayFromList(values);
}

Result<Value> DateTimeFormatPrototype_formatToParts(NativeCall& call) {
  DateTimeFormatObject* dtf = TRY(thisIntlObject<DateTimeFormatObject>(call.thisValue, "Intl.DateTimeFormat.prototype.formatToParts"));
  Formattable x = TRY(formattableArgument(call.args[0]));
  std::vector<DatePart> parts = TRY(formatParts(*dtf, x));
  return Value(partsArray(parts, false));
}

// 11.5.9 PartitionDateTimeRangePattern ( dateTimeFormat, x, y )
Result<std::vector<DatePart>> rangeParts(const DateTimeFormatObject& dtf, const Formattable& fx, const Formattable& fy) {
  if ((fx.kind != ValueKind::Number || fy.kind != ValueKind::Number) && fx.kind != fy.kind)
    return ThrowTypeError("Intl.DateTimeFormat: formatRange needs two values of the same kind");
  auto [pattern, utc] = TRY(patternForValue(dtf, fx));
  TRY(patternForValue(dtf, fy));
  double x = fx.kind == ValueKind::Number ? timeClip(fx.epochMs) : fx.epochMs;
  double y = fy.kind == ValueKind::Number ? timeClip(fy.epochMs) : fy.epochMs;
  LocalTime a = toLocalTime(x + double(utc ? 0 : offsetMsAt(dtf, x))), b = toLocalTime(y + double(utc ? 0 : offsetMsAt(dtf, y)));
  std::vector<Token> tokens = parsePattern(pattern);
  unsigned present = 0;
  for (const Token& t : tokens)
    if (t.field && fieldOf(t.ch) >= 0) present |= 1u << fieldOf(t.ch);
  // The greatest field that differs (as an interval-format "greatest difference" letter).
  char16_t diff = 0;
  bool twelve = false;
  for (const Token& t : tokens) twelve |= t.field && twelveHour(t.ch);
  if ((present & (1u << Era)) && ((a.year > 0) != (b.year > 0))) diff = u'G';
  else if ((present & (1u << Year)) && a.year != b.year) diff = u'y';
  else if ((present & (1u << Month)) && (a.year != b.year || a.month != b.month)) diff = u'M';
  else if ((present & ((1u << Day) | (1u << Weekday))) && (a.year != b.year || a.month != b.month || a.day != b.day)) diff = u'd';
  else if ((present & (1u << Hour)) && twelve && (a.hour < 12) != (b.hour < 12)) diff = u'a';
  else if ((present & (1u << Hour)) && a.hour != b.hour) diff = twelve ? u'h' : u'H';
  else if ((present & (1u << Minute)) && a.minute != b.minute) diff = u'm';
  else if ((present & (1u << Second)) && a.second != b.second) diff = u's';
  else if ((present & (1u << Fraction)) && a.millisecond != b.millisecond) diff = u'S';
  // Any coarser field that is not displayed but differs still makes the dates distinct.
  bool sameDay = a.year == b.year && a.month == b.month && a.day == b.day;
  if (!diff && !sameDay && !(present & kDateMask)) diff = u'd';
  std::vector<DatePart> out;
  if (!diff) {
    formatPatternInto(out, dtf, pattern, x, 0, utc);
    return out;
  }
  std::vector<std::u16string_view> list = items(dateField(dtf.dataLocale, FIntervals));
  std::u16string fallback = list.empty() ? u"{0} \u2013 {1}" : std::u16string(list[0]);
  const Generator& g = generatorFor(dtf.dataLocale);
  // The interval pattern for the pattern's fields (some of them: mask) and the greatest difference, or "".
  auto intervalFor = [&](unsigned mask) -> std::u16string_view {
    if (diff == u's' || diff == u'S' || (present & (1u << Zone))) return {};
    Skeleton k = skeletonOf(pattern);
    Best best = getBestRaw(g, k, k.mask() & mask & ~(1u << Zone));
    if (!best.match || best.distance.missing != 0 || best.distance.extra != 0) return {};
    std::u16string key(best.match->key);
    for (size_t i = 1; i < list.size(); i++) {
      std::u16string_view item = list[i];
      size_t slash = item.find(u'/'), eq = item.find(u'=');
      if (item.substr(0, slash) == key && item.substr(slash + 1, eq - slash - 1) == std::u16string_view(&diff, 1)) return item.substr(eq + 1);
    }
    return {};
  };
  // Formats x and y with an interval pattern: the first part (until a field repeats) with x, the rest with y.
  auto appendInterval = [&](std::u16string_view interval) {
    std::u16string adjusted = applyHourCycle(g, interval, dtf.hourCycle.empty() ? "h23" : dtf.hourCycle);
    std::vector<Token> it = parsePattern(adjusted);
    for (Token& t : it) {
      if (!t.field) continue;
      for (const Token& p : tokens)
        if (p.field && fieldOf(p.ch) == fieldOf(t.ch) && fieldOf(t.ch) != Hour) t.count = p.count, t.ch = p.ch;
    }
    unsigned seen = 0;
    size_t split = it.size();
    for (size_t i = 0; i < it.size(); i++) {
      if (!it[i].field) continue;
      unsigned bit = 1u << fieldOf(it[i].ch);
      if (seen & bit) {
        split = i;
        break;
      }
      seen |= bit;
    }
    size_t firstEnd = split;
    while (firstEnd > 0 && !it[firstEnd - 1].field) firstEnd--;
    std::vector<Token> first(it.begin(), it.begin() + long(firstEnd)), second(it.begin() + long(split), it.end());
    std::u16string separator;
    for (size_t i = firstEnd; i < split; i++) separator += it[i].text;
    // A field that only one half has is shared, and so is a literal unless both its neighbours are in the half.
    auto fieldsOf = [](const std::vector<Token>& ts) {
      unsigned m = 0;
      for (const Token& t : ts)
        if (t.field && fieldOf(t.ch) >= 0) m |= 1u << fieldOf(t.ch);
      return m;
    };
    unsigned firstFields = fieldsOf(first), secondFields = fieldsOf(second);
    auto half = [&](const std::vector<Token>& ts, double at, int source, unsigned otherFields) {
      std::vector<DatePart> parts;
      formatPatternInto(parts, dtf, patternString(ts), at, source, utc);
      static const std::pair<const char16_t*, int> kinds[] = {
          {u"era", Era}, {u"year", Year}, {u"month", Month}, {u"weekday", Weekday}, {u"day", Day}, {u"dayPeriod", DayPeriod},
          {u"hour", Hour}, {u"minute", Minute}, {u"second", Second}, {u"fractionalSecond", Fraction}, {u"timeZoneName", Zone}};
      for (DatePart& part : parts) {
        for (const auto& [name, field] : kinds)
          if (std::u16string_view(part.type) == name && !(otherFields & (1u << field))) part.source = 0;
      }
      for (size_t i = 0; i < parts.size(); i++) {
        if (std::u16string_view(parts[i].type) != u"literal") continue;
        bool before = i > 0 && parts[i - 1].source != 0, after = i + 1 < parts.size() && parts[i + 1].source != 0;
        if (!(before && after)) parts[i].source = 0;
      }
      out.insert(out.end(), parts.begin(), parts.end());
    };
    half(first, x, 1, secondFields);
    if (!separator.empty()) out.push_back({u"literal", separator, 0});
    half(second, y, 2, firstFields);
  };
  if (std::u16string_view interval = intervalFor(~0u); !interval.empty()) {
    appendInterval(interval);
    return out;
  }
  // The same day, a difference in the time: the date once, then the time interval (as ICU's DateIntervalFormat).
  bool timeDiff = diff == u'a' || diff == u'h' || diff == u'H' || diff == u'm' || diff == u's' || diff == u'S';
  if (timeDiff && (present & kDateMask) && (present & kTimeMask) && sameDay) {
    std::u16string_view interval = intervalFor(kTimeMask);
    {
      Skeleton k = skeletonOf(pattern), dateOnly, timeOnly;
      for (int f = 0; f < kFieldCount; f++) {
        if ((kDateMask & (1u << f)) && k.ch[f]) dateOnly.set(k.ch[f], k.len[f]);
        if ((kTimeMask & (1u << f)) && k.ch[f]) timeOnly.set(k.ch[f], k.len[f]);
      }
      int style = 3;
      if (k.len[Month] == 4) style = k.ch[Weekday] ? 0 : 1;
      else if (k.len[Month] == 3) style = 2;
      std::u16string_view glue = g.atTime[style];
      size_t p0 = glue.find(u"{0}"), p1 = glue.find(u"{1}");
      for (size_t i = 0; i < glue.size();) {
        if (i == p1) {
          formatPatternInto(out, dtf, getBestPattern(g, dateOnly), x, 0, utc);
          i += 3;
        } else if (i == p0) {
          if (!interval.empty()) {
            appendInterval(interval);
          } else {
            // The interval fallback over the time part alone.
            std::u16string time = getBestPattern(g, timeOnly);
            size_t f0 = fallback.find(u"{0}"), f1 = fallback.find(u"{1}");
            formatPatternInto(out, dtf, time, x, 1, utc);
            out.push_back({u"literal", fallback.substr(f0 + 3, f1 - f0 - 3), 0});
            formatPatternInto(out, dtf, time, y, 2, utc);
          }
          i += 3;
        } else {
          size_t next = std::min(p0 > i ? p0 : glue.size(), p1 > i ? p1 : glue.size());
          std::vector<Token> lit = parsePattern(glue.substr(i, next - i));
          for (const Token& t : lit)
            if (!t.field) out.push_back({u"literal", t.text, 0});
          i = next;
        }
      }
      return out;
    }
  }
  // FormatDateTimePattern for both, joined by the fallback pattern.
  size_t p0 = fallback.find(u"{0}"), p1 = fallback.find(u"{1}");
  std::u16string before = fallback.substr(0, std::min(p0, p1)), middle = fallback.substr(std::min(p0, p1) + 3, std::max(p0, p1) - std::min(p0, p1) - 3);
  if (!before.empty()) out.push_back({u"literal", before, 0});
  formatPatternInto(out, dtf, pattern, p0 < p1 ? x : y, p0 < p1 ? 1 : 2, utc);
  out.push_back({u"literal", middle, 0});
  formatPatternInto(out, dtf, pattern, p0 < p1 ? y : x, p0 < p1 ? 2 : 1, utc);
  std::u16string after = fallback.substr(std::max(p0, p1) + 3);
  if (!after.empty()) out.push_back({u"literal", after, 0});
  return out;
}

Result<std::pair<Formattable, Formattable>> rangeArgs(NativeCall& call) {
  if (call.args[0].isUndefined() || call.args[1].isUndefined()) return ThrowTypeError("Intl.DateTimeFormat: formatRange needs two dates");
  Formattable x = TRY(toFormattable(call.args[0]));
  Formattable y = TRY(toFormattable(call.args[1]));
  return std::make_pair(x, y);
}

Result<Value> DateTimeFormatPrototype_formatRange(NativeCall& call) {
  DateTimeFormatObject* dtf = TRY(thisIntlObject<DateTimeFormatObject>(call.thisValue, "Intl.DateTimeFormat.prototype.formatRange"));
  auto [x, y] = TRY(rangeArgs(call));
  std::vector<DatePart> parts = TRY(rangeParts(*dtf, x, y));
  return Value(newString(joinParts(parts)));
}

Result<Value> DateTimeFormatPrototype_formatRangeToParts(NativeCall& call) {
  DateTimeFormatObject* dtf = TRY(thisIntlObject<DateTimeFormatObject>(call.thisValue, "Intl.DateTimeFormat.prototype.formatRangeToParts"));
  auto [x, y] = TRY(rangeArgs(call));
  std::vector<DatePart> parts = TRY(rangeParts(*dtf, x, y));
  return Value(partsArray(parts, true));
}

// 11.3.7 Intl.DateTimeFormat.prototype.resolvedOptions ( )
Result<Value> DateTimeFormatPrototype_resolvedOptions(NativeCall& call) {
  DateTimeFormatObject* dtf = TRY(unwrapDateTimeFormat(call.thisValue, "Intl.DateTimeFormat.prototype.resolvedOptions"));
  Object* o = builtins::newPlainObject();
  addProperty(o, u"locale", jsString(dtf->locale));
  addProperty(o, u"calendar", jsString(dtf->calendar));
  addProperty(o, u"numberingSystem", jsString(dtf->numberingSystem));
  addProperty(o, u"timeZone", jsString(dtf->timeZone.id));
  if (!dtf->hourCycle.empty()) {
    addProperty(o, u"hourCycle", jsString(dtf->hourCycle));
    addProperty(o, u"hour12", boolean(dtf->hourCycle == "h11" || dtf->hourCycle == "h12"));
  }
  if (!dtf->dateStyle && !dtf->timeStyle) {
    std::vector<Token> tokens = parsePattern(dtf->pattern);
    auto find = [&](std::initializer_list<char16_t> chars) -> const Token* {
      for (const Token& t : tokens)
        if (t.field && std::find(chars.begin(), chars.end(), t.ch) != chars.end()) return &t;
      return nullptr;
    };
    auto text = [](int n) { return n == 4 ? "long" : n == 5 ? "narrow" : "short"; };
    auto numeric = [](int n) { return n == 2 ? "2-digit" : "numeric"; };
    if (const Token* t = find({u'E', u'c', u'e'})) addProperty(o, u"weekday", jsString(text(t->count)));
    if (const Token* t = find({u'G'})) addProperty(o, u"era", jsString(text(t->count)));
    if (const Token* t = find({u'y', u'Y', u'u', u'r'})) addProperty(o, u"year", jsString(numeric(t->count)));
    if (const Token* t = find({u'M', u'L'})) addProperty(o, u"month", jsString(t->count <= 2 ? numeric(t->count) : text(t->count)));
    if (const Token* t = find({u'd'})) addProperty(o, u"day", jsString(numeric(t->count)));
    if (const Token* t = find({u'B', u'b'})) addProperty(o, u"dayPeriod", jsString(text(t->count)));
    if (const Token* t = find({u'h', u'H', u'K', u'k'})) addProperty(o, u"hour", jsString(numeric(t->count)));
    if (const Token* t = find({u'm'})) addProperty(o, u"minute", jsString(numeric(t->count)));
    if (const Token* t = find({u's'})) addProperty(o, u"second", jsString(numeric(t->count)));
    if (const Token* t = find({u'S'})) addProperty(o, u"fractionalSecondDigits", num(t->count));
    if (const Token* t = find({u'z', u'O', u'v'})) {
      const char* name = t->ch == u'z' ? (t->count >= 4 ? "long" : "short")
                         : t->ch == u'O' ? (t->count >= 4 ? "longOffset" : "shortOffset")
                                         : (t->count >= 4 ? "longGeneric" : "shortGeneric");
      addProperty(o, u"timeZoneName", jsString(name));
    }
  }
  if (dtf->dateStyle) addProperty(o, u"dateStyle", jsString(*dtf->dateStyle));
  if (dtf->timeStyle) addProperty(o, u"timeStyle", jsString(*dtf->timeStyle));
  return Value(o);
}

}  // namespace

Result<Value> dateToLocaleString(double timeValue, Value locales, Value options, DateRequired required) {
  if (std::isnan(timeValue)) return str(u"Invalid Date");
  DateRequired2 r = required == DateRequired::Date ? DateRequired2::Date : required == DateRequired::Time ? DateRequired2::Time : DateRequired2::Any;
  DateDefaults d = required == DateRequired::Date ? DateDefaults::Date : required == DateRequired::Time ? DateDefaults::Time : DateDefaults::All;
  DateTimeFormatObject* dtf = TRY(CreateDateTimeFormat(builtins::intrinsic(Intrinsic::IntlDateTimeFormat), locales, options, r, d));
  std::vector<DatePart> parts = TRY(formatParts(*dtf, timeValue));
  return Value(newString(joinParts(parts)));
}

// Temporal.{Instant,PlainDate,PlainDateTime,PlainMonthDay,PlainTime,PlainYearMonth}.prototype.toLocaleString:
// CreateDateTimeFormat with the type's required and defaults, then FormatDateTime.
Result<Value> temporalToLocaleString(Value temporalValue, Value locales, Value options) {
  Formattable x = TRY(toFormattable(temporalValue));
  DateRequired2 required = DateRequired2::Any;
  DateDefaults defaults = DateDefaults::All;
  switch (x.kind) {
    case ValueKind::PlainDate:
    case ValueKind::PlainYearMonth:
    case ValueKind::PlainMonthDay:
      required = DateRequired2::Date;
      defaults = DateDefaults::Date;
      break;
    case ValueKind::PlainTime:
      required = DateRequired2::Time;
      defaults = DateDefaults::Time;
      break;
    default:
      break;
  }
  DateTimeFormatObject* dtf =
      TRY(CreateDateTimeFormat(builtins::intrinsic(Intrinsic::IntlDateTimeFormat), locales, options, required, defaults));
  std::vector<DatePart> parts = TRY(formatParts(*dtf, x, true));
  return Value(newString(joinParts(parts)));
}

// Temporal.ZonedDateTime.prototype.toLocaleString: its instant, in its own time zone, in a calendar that matches.
Result<Value> zonedDateTimeToLocaleString(Value zonedDateTime, Value locales, Value options) {
  auto* zoned = temporal::asTemporal<temporal::ZonedDateTimeObject>(zonedDateTime);
  DateTimeFormatObject* dtf = TRY(CreateDateTimeFormat(builtins::intrinsic(Intrinsic::IntlDateTimeFormat), locales, options,
                                                       DateRequired2::Any, DateDefaults::All, &zoned->timeZone));
  std::string calendar = utf16ToUtf8(zoned->calendar);
  if (calendar != "iso8601" && calendar != dtf->calendar)
    return ThrowRangeError("Temporal.ZonedDateTime.prototype.toLocaleString: the calendar " + calendar + " is not the format's " +
                           dtf->calendar);
  temporal::EpochNanoseconds ns = zoned->epochNanoseconds;
  temporal::EpochNanoseconds ms = ns / 1000000 - (ns % 1000000 < 0 ? 1 : 0);
  // FormatDateTime(dateTimeFormat, instant) with the format this CreateDateTimeFormat made (~any~, ~all~, its time
  // zone): [[DateTimeFormat]] itself, as for a time value.
  std::vector<DatePart> parts = TRY(formatParts(*dtf, double(int64_t(ms))));
  return Value(newString(joinParts(parts)));
}

void installDateTimeFormat(Realm& realm, Object* intl) {
  Object* proto = nullptr;
  BuiltinFunction* ctor = defineIntlConstructor(realm, intl, Intrinsic::IntlDateTimeFormat, DateTimeFormat_construct, 0,
                                                u"DateTimeFormat", Intrinsic::IntlDateTimeFormatPrototype, &proto);
  defineFunction(realm, ctor, PropertyKey(u"supportedLocalesOf"), 1, DateTimeFormat_supportedLocalesOf);
  defineAccessor(realm, proto, PropertyKey(u"format"), DateTimeFormatPrototype_format, nullptr);
  defineFunction(realm, proto, PropertyKey(u"formatRange"), 2, DateTimeFormatPrototype_formatRange);
  defineFunction(realm, proto, PropertyKey(u"formatRangeToParts"), 2, DateTimeFormatPrototype_formatRangeToParts);
  defineFunction(realm, proto, PropertyKey(u"formatToParts"), 1, DateTimeFormatPrototype_formatToParts);
  defineFunction(realm, proto, PropertyKey(u"resolvedOptions"), 0, DateTimeFormatPrototype_resolvedOptions);
}

}  // namespace snowman::intl
