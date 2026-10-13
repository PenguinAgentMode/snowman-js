// Intl.NumberFormat (ECMA-402 15): options, PartitionNumberPattern over the generated locale data
// (number_data.inc: symbols, grouping and sign/style templates; compact, unit and currency name templates by plural
// category), formatToParts and formatRange.
#include <algorithm>
#include <cmath>

#include "builtins/intl/intl.h"
#include "builtins/intl/services.h"
#include "runtime/symbol.h"
#include "unicode/unicode.h"

namespace snowman::intl {

namespace {

struct DataEntry {
  const char* key;
  const char16_t* value;
};
struct CurrencyDigitsEntry;
#include "builtins/intl/number_data.inc"

constexpr const char* kRelevantKeys[] = {"nu"};

std::vector<std::string> numberKeyValues(const std::string& locale, std::string_view key) {
  if (key != "nu") return {};
  std::vector<std::string> out{data::defaultNumberingSystem(locale)};
  for (const std::string& nu : data::numberingSystems())
    if (nu != out[0]) out.push_back(nu);
  return out;
}

const ServiceData kNumberFormatData{data::locales, numberKeyValues};

template <size_t N>
const char16_t* find(const DataEntry (&table)[N], const std::string& key) {
  const DataEntry* end = table + N;
  const DataEntry* it = std::lower_bound(table, end, key, [](const DataEntry& e, const std::string& k) { return std::string_view(e.key) < k; });
  return it != end && key == it->key ? it->value : nullptr;
}

// The value for "locale/suffix" along the locale's truncation chain, then "/suffix".
template <size_t N>
const char16_t* inherited(const DataEntry (&table)[N], std::string_view locale, std::string_view suffix) {
  for (const std::string& c : data::lookupChain(locale)) {
    if (const char16_t* v = find(table, c + "/" + std::string(suffix))) return v;
  }
  return find(table, "/" + std::string(suffix));
}

std::vector<std::u16string_view> fields(std::u16string_view s, char16_t sep) {
  std::vector<std::u16string_view> out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(sep, start);
    out.push_back(s.substr(start, pos == std::u16string_view::npos ? std::u16string_view::npos : pos - start));
    if (pos == std::u16string_view::npos) return out;
    start = pos + 1;
  }
}

struct Symbols {
  std::u16string decimal, group, minus, plus, percent, exponential, exponentMinus, infinity, nan, approximately;
};

Symbols symbolsFor(const std::string& locale, const std::string& nu) {
  const char16_t* v = inherited(kNumberSymbols, locale, nu);
  if (!v || data::defaultNumberingSystem(locale) != nu) {
    // A numbering system that is not the locale's own: the root symbols of that numbering system (latn: the locale's).
    if (nu != "latn") v = find(kNumberSymbols, "/" + nu);
    if (!v) v = inherited(kNumberSymbols, locale, "latn");
  }
  std::vector<std::u16string_view> f = fields(v, u'\x1f');
  f.resize(10);
  return {std::u16string(f[0]), std::u16string(f[1]), std::u16string(f[2]), std::u16string(f[3]), std::u16string(f[4]),
          std::u16string(f[5]), std::u16string(f[6]), std::u16string(f[7]), std::u16string(f[8]), std::u16string(f[9])};
}

std::u16string_view patternField(const std::string& locale, int field) {
  return inherited(kNumberPatterns, locale, std::to_string(field));
}

// A template's pieces: a type letter and its text.
struct Piece {
  char16_t type;
  std::u16string_view text;
};
std::vector<Piece> pieces(std::u16string_view t) {
  std::vector<Piece> out;
  if (t.empty()) return out;
  for (std::u16string_view p : fields(t, u'\x1e')) {
    if (!p.empty()) out.push_back({p[0], p.substr(1)});
  }
  return out;
}

const char16_t* pieceTypeName(char16_t t) {
  switch (t) {
    case u'L': return u"literal";
    case u'M': return u"minusSign";
    case u'P': return u"plusSign";
    case u'%': return u"percentSign";
    case u'U': return u"unit";
    case u'K': return u"compact";
    case u'C': return u"currency";
    default: return u"literal";
  }
}

// The template for a plural category from "c<template>\x1d..." (falling back to other).
std::u16string_view byCategory(std::u16string_view list, char category) {
  std::u16string_view other;
  for (std::u16string_view entry : fields(list, u'\x1d')) {
    if (entry.empty()) continue;
    if (entry[0] == char16_t(category)) return entry.substr(1);
    if (entry[0] == u'x') other = entry.substr(1);
  }
  return other;
}

// The compact data of a locale (rich locales; others inherit, ultimately from en).
std::u16string_view compactData(const std::string& locale, bool longDisplay) {
  const char16_t* v = nullptr;
  for (const std::string& c : data::lookupChain(locale))
    if (!v) v = find(kCompactPatterns, c);
  if (!v) v = find(kCompactPatterns, "");
  std::u16string_view s(v);
  size_t split = s.find(u"\x1f\x1f");
  return longDisplay ? s.substr(split + 2) : s.substr(0, split);
}

// The compact exponent and templates for a magnitude (3..14).
std::pair<int, std::u16string_view> compactPattern(const NumberFormatObject& nf, int64_t magnitude) {
  if (magnitude < 3) return {0, {}};
  if (magnitude > 14) magnitude = 14;
  std::vector<std::u16string_view> mags = fields(compactData(nf.dataLocale, nf.compactDisplay == "long"), u'\x1f');
  std::u16string_view m = mags[size_t(magnitude - 3)];
  size_t gs = m.find(u'\x1d');
  int exponent = 0;
  for (char16_t c : m.substr(0, gs)) exponent = exponent * 10 + (c - u'0');
  if (exponent == 0) return {0, {}};
  return {exponent, m.substr(gs + 1)};
}

std::u16string_view unitData(const std::string& locale, const std::string& unit, const std::string& display) {
  const char16_t* v = inherited(kUnitPatterns, locale, unit);
  if (!v) return {};
  std::vector<std::u16string_view> w = fields(v, u'\x1f');
  w.resize(3);
  if (display == "narrow") return w[1].empty() ? w[0] : w[1];
  if (display == "long") return w[2];
  return w[0];
}

struct CurrencyNames {
  std::u16string symbol, narrow;
  std::u16string_view names;
};
CurrencyNames currencyNames(const std::string& locale, const std::string& code) {
  const char16_t* v = nullptr;
  for (const std::string& c : data::lookupChain(locale))
    if (!v) v = find(kCurrencyNames, c + "/" + code);
  if (!v) v = find(kCurrencyNames, "/" + code);
  std::u16string c16 = builtins::toU16(code);
  if (!v) return {c16, c16, {}};
  std::vector<std::u16string_view> f = fields(v, u'\x1f');
  f.resize(3);
  return {std::u16string(f[0]), std::u16string(f[1]), f[2]};
}

int currencyDigits(const std::string& code) {
  for (const CurrencyDigitsEntry& e : kCurrencyDigits)
    if (code == e.code) return e.digits;
  return 2;
}

bool isWellFormedCurrencyCode(const std::string& c) {
  return c.size() == 3 && std::all_of(c.begin(), c.end(), [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); });
}

bool isWellFormedUnitIdentifier(const std::string& unit) {
  if (data::isSanctionedSimpleUnit(unit)) return true;
  size_t per = unit.find("-per-");
  if (per == std::string::npos) return false;
  return data::isSanctionedSimpleUnit(unit.substr(0, per)) && data::isSanctionedSimpleUnit(unit.substr(per + 5));
}

// ---- number parts ----

std::u16string transliterate(const std::string& ascii, const char16_t* digits) {
  std::u16string out;
  for (char c : ascii) {
    if (c >= '0' && c <= '9' && digits) {
      // digits holds ten code points (one or two code units each).
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

void appendGroupedInteger(std::vector<NumberPart>& parts, const NumberFormatObject& nf, const std::string& integer,
                          const Symbols& sym, const char16_t* digits) {
  std::u16string_view g = patternField(nf.dataLocale, 0);
  int primary = g.size() > 0 ? g[0] - u'0' : 3, secondary = g.size() > 1 ? g[1] - u'0' : 3;
  int minGrouping = g.size() > 2 ? g[2] - u'0' : 1;
  if (nf.useGrouping == "always") minGrouping = 1;
  else if (nf.useGrouping == "min2") minGrouping = 2;
  bool group = nf.useGrouping != "false" && int(integer.size()) >= primary + minGrouping;
  if (!group) {
    parts.push_back({u"integer", transliterate(integer, digits)});
    return;
  }
  std::vector<std::string> groups;
  std::string rest = integer;
  groups.push_back(rest.substr(rest.size() - size_t(primary)));
  rest.resize(rest.size() - size_t(primary));
  while (!rest.empty()) {
    size_t n = std::min(rest.size(), size_t(secondary));
    groups.push_back(rest.substr(rest.size() - n));
    rest.resize(rest.size() - n);
  }
  for (size_t i = groups.size(); i-- > 0;) {
    parts.push_back({u"integer", transliterate(groups[i], digits)});
    if (i) parts.push_back({u"group", sym.group});
  }
}

struct Formatted {
  std::vector<NumberPart> number;  // the digits (and exponent): the {number} of the templates
  Decimal rounded;                 // [[RoundedNumber]] (signed), or the non-finite input
  std::string integer, fraction;   // for plural rules
  int compactExponent = 0;
  std::u16string_view compactTemplates;
};

// ComputeExponentForMagnitude ( numberFormat, magnitude )
int exponentForMagnitude(const NumberFormatObject& nf, int64_t magnitude) {
  if (nf.notation == "scientific") return int(magnitude);
  if (nf.notation == "engineering") return int(std::floor(double(magnitude) / 3) * 3);
  if (nf.notation == "compact") return compactPattern(nf, magnitude).first;
  return 0;
}

Formatted formatNumber(const NumberFormatObject& nf, const Decimal& input) {
  Formatted f;
  Symbols sym = symbolsFor(nf.dataLocale, nf.numberingSystem);
  const char16_t* digits = nf.numberingSystem == "latn" ? nullptr : data::numberingSystemDigits(nf.numberingSystem);
  if (input.isNaN()) {
    f.number.push_back({u"nan", sym.nan});
    f.rounded = input;
    f.integer = "0";
    return f;
  }
  if (!input.isFinite()) {
    f.number.push_back({u"infinity", sym.infinity});
    f.rounded = input;
    f.integer = "0";
    return f;
  }
  Decimal x = input;
  if (nf.style == "percent") x = x.scaled(2);
  int exponent = 0;
  // ComputeExponent ( numberFormat, x )
  if (!x.isZero() && nf.notation != "standard") {
    int64_t magnitude = x.magnitude();
    exponent = exponentForMagnitude(nf, magnitude);
    RawFormat r = FormatNumericToString(nf.digits, x.scaled(-exponent));
    if (!r.rounded.isZero() && r.rounded.magnitude() != magnitude - exponent) exponent = exponentForMagnitude(nf, magnitude + 1);
  }
  RawFormat r = FormatNumericToString(nf.digits, x.scaled(-exponent));
  f.rounded = r.rounded;
  f.integer = r.integer;
  f.fraction = r.fraction;
  appendGroupedInteger(f.number, nf, r.integer, sym, digits);
  if (!r.fraction.empty()) {
    f.number.push_back({u"decimal", sym.decimal});
    f.number.push_back({u"fraction", transliterate(r.fraction, digits)});
  }
  if (nf.notation == "scientific" || nf.notation == "engineering") {
    f.compactExponent = exponent;
    f.number.push_back({u"exponentSeparator", sym.exponential});
    if (exponent < 0) f.number.push_back({u"exponentMinusSign", sym.exponentMinus});
    f.number.push_back({u"exponentInteger", transliterate(std::to_string(std::abs(exponent)), digits)});
  } else if (nf.notation == "compact" && exponent != 0) {
    f.compactExponent = exponent;
    f.compactTemplates = compactPattern(nf, x.magnitude() >= 3 ? std::max<int64_t>(x.magnitude(), exponent) : 3).second;
    // The pattern of the exponent actually used: find the magnitude whose exponent it is.
    for (int64_t m = 3; m <= 14; m++) {
      auto [e, t] = compactPattern(nf, m);
      if (e == exponent && int64_t(r.integer.size()) - 1 + exponent == m) {
        f.compactTemplates = t;
        break;
      }
    }
  }
  return f;
}

// Which sign template: 0 zero (no sign), 1 plus, 2 minus.
int signTemplate(const NumberFormatObject& nf, const Decimal& x) {
  bool nan = x.isNaN();
  bool negative = x.kind == Decimal::Kind::NegativeInfinity || (x.isFinite() && x.negative);
  bool zero = x.isZero();
  bool positive = x.kind == Decimal::Kind::PositiveInfinity || (x.isFinite() && !x.negative && !zero);
  const std::string& s = nf.signDisplay;
  if (s == "never") return 0;
  if (s == "auto") return negative ? 2 : 0;
  if (s == "always") return negative ? 2 : 1;
  if (s == "exceptZero") {
    if (nan || zero) return 0;
    return positive ? 1 : 2;
  }
  // negative
  return negative && !zero ? 2 : 0;
}

void appendTemplate(std::vector<NumberPart>& out, std::u16string_view tmpl, const std::vector<NumberPart>& number,
                    std::u16string_view currency = {}) {
  for (const Piece& p : pieces(tmpl)) {
    if (p.type == u'N') {
      out.insert(out.end(), number.begin(), number.end());
    } else if (p.type == u'C') {
      out.push_back({u"currency", std::u16string(currency)});
    } else {
      out.push_back({pieceTypeName(p.type), std::u16string(p.text)});
    }
  }
}

std::u16string_view signVariant(std::u16string_view field, int sign) {
  std::vector<std::u16string_view> f = fields(field, u'\x1f');
  return f[size_t(std::min<int>(sign, int(f.size()) - 1))];
}

bool isLetter(char16_t c) {
  if ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z')) return true;
  if (c < 0x80) return false;
  std::optional<unicode::Property> p = unicode::lookupLoneProperty("L");
  std::span<const unicode::CodePointRange> ranges = unicode::propertyCodePoints(*p);
  auto it = std::upper_bound(ranges.begin(), ranges.end(), char32_t(c), [](char32_t v, const unicode::CodePointRange& r) { return v < r.first; });
  return it != ranges.begin() && c <= (it - 1)->last;
}

}  // namespace

void NumberFormatObject::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(boundFormat);
}

char numberFormatPluralCategory(const NumberFormatObject& nf, const Decimal& x, bool ordinal) {
  Formatted f = formatNumber(nf, x);
  return pluralCategory(pluralRulesFor(nf.dataLocale, ordinal), f.integer, f.fraction, f.compactExponent);
}

std::vector<NumberPart> PartitionNumberPattern(const NumberFormatObject& nf, const Decimal& x) {
  Formatted f = formatNumber(nf, x);
  char category = pluralCategory(pluralRulesFor(nf.dataLocale, false), f.integer, f.fraction, f.compactExponent);
  std::vector<NumberPart> number = f.number;
  if (!f.compactTemplates.empty()) {
    std::vector<NumberPart> compacted;
    appendTemplate(compacted, byCategory(f.compactTemplates, category), number);
    number = std::move(compacted);
  }
  int sign = signTemplate(nf, f.rounded);
  std::vector<NumberPart> out;
  if (nf.style == "decimal" || nf.style == "percent") {
    appendTemplate(out, signVariant(patternField(nf.dataLocale, nf.style == "decimal" ? 1 : 2), sign), number);
  } else if (nf.style == "currency") {
    CurrencyNames names = currencyNames(nf.dataLocale, nf.currency);
    if (nf.currencyDisplay == "name") {
      std::u16string_view name = byCategory(names.names, category);
      if (name.empty()) name = names.symbol;
      appendTemplate(out, signVariant(patternField(nf.dataLocale, 7), sign), number, name.empty() ? builtins::toU16(nf.currency) : std::u16string(name));
    } else {
      std::u16string display = nf.currencyDisplay == "code" ? builtins::toU16(nf.currency)
                               : nf.currencyDisplay == "narrowSymbol" ? names.narrow : names.symbol;
      bool accounting = nf.currencySign == "accounting";
      // The symbol template when the display string's character next to the number is not a letter.
      std::u16string_view symbolTemplate = signVariant(patternField(nf.dataLocale, accounting ? 5 : 3), sign);
      std::vector<Piece> ps = pieces(symbolTemplate);
      bool currencyFirst = true;
      for (const Piece& p : ps) {
        if (p.type == u'N') break;
        if (p.type == u'C') {
          currencyFirst = true;
          break;
        }
      }
      size_t cpos = 0, npos = 0;
      for (size_t i = 0; i < ps.size(); i++) {
        if (ps[i].type == u'C') cpos = i;
        if (ps[i].type == u'N') npos = i;
      }
      currencyFirst = cpos < npos;
      char16_t adjacent = display.empty() ? u' ' : currencyFirst ? display.back() : display.front();
      std::u16string_view tmpl = isLetter(adjacent) ? signVariant(patternField(nf.dataLocale, accounting ? 6 : 4), sign) : symbolTemplate;
      appendTemplate(out, tmpl, number, display);
    }
  } else {
    // unit: the unit template around the signed number.
    std::vector<NumberPart> signedNumber;
    appendTemplate(signedNumber, signVariant(patternField(nf.dataLocale, 1), sign), number);
    std::u16string_view unitTemplates = unitData(nf.dataLocale, nf.unit, nf.unitDisplay);
    if (!unitTemplates.empty()) {
      appendTemplate(out, byCategory(unitTemplates, category), signedNumber);
    } else {
      // X-per-Y: X's template followed by "/" and Y's short unit text.
      size_t per = nf.unit.find("-per-");
      std::string numerator = nf.unit.substr(0, per), denominator = nf.unit.substr(per + 5);
      appendTemplate(out, byCategory(unitData(nf.dataLocale, numerator, nf.unitDisplay), category), signedNumber);
      std::u16string denom;
      for (const Piece& p : pieces(byCategory(unitData(nf.dataLocale, denominator, "short"), 'o')))
        if (p.type == u'U') denom = std::u16string(p.text);
      if (denom.empty()) denom = builtins::toU16(denominator);
      bool appended = false;
      for (NumberPart& part : out) {
        if (std::u16string_view(part.type) == u"unit") {
          part.value += u"/" + denom;
          appended = true;
        }
      }
      if (!appended) out.push_back({u"unit", u"/" + denom});
    }
  }
  return out;
}

std::u16string FormatNumeric(const NumberFormatObject& nf, const Decimal& x) {
  std::u16string out;
  for (const NumberPart& p : PartitionNumberPattern(nf, x)) out += p.value;
  return out;
}

namespace {

Object* partsToArray(const std::vector<NumberPart>& parts, const std::vector<const char16_t*>* sources = nullptr) {
  ValueVector values;
  for (size_t i = 0; i < parts.size(); i++) {
    Object* o = builtins::newPlainObject();
    addProperty(o, u"type", str(parts[i].type));
    addProperty(o, u"value", Value(newString(parts[i].value)));
    if (sources) addProperty(o, u"source", str((*sources)[i]));
    values.push_back(Value(o));
  }
  return CreateArrayFromList(values);
}

// 15.5.19 PartitionNumberRangePattern ( numberFormat, x, y ) with ICU-like collapsing of identical affixes.
Result<std::pair<std::vector<NumberPart>, std::vector<const char16_t*>>> partitionRange(const NumberFormatObject& nf, const Decimal& x,
                                                                                       const Decimal& y) {
  if (x.isNaN() || y.isNaN()) return ThrowRangeError("Intl.NumberFormat: formatRange with NaN");
  std::vector<NumberPart> xs = PartitionNumberPattern(nf, x), ys = PartitionNumberPattern(nf, y);
  std::vector<NumberPart> out;
  std::vector<const char16_t*> sources;
  auto concat = [](const std::vector<NumberPart>& ps) {
    std::u16string s;
    for (const NumberPart& p : ps) s += p.value;
    return s;
  };
  Symbols sym = symbolsFor(nf.dataLocale, nf.numberingSystem);
  if (concat(xs) == concat(ys)) {
    // FormatApproximately: the approximately sign before the first non-literal part.
    size_t at = 0;
    while (at < xs.size() && std::u16string_view(xs[at].type) == u"literal") at++;
    xs.insert(xs.begin() + long(at), NumberPart{u"approximatelySign", sym.approximately});
    sources.assign(xs.size(), u"shared");
    return std::make_pair(xs, sources);
  }
  auto isNumberPart = [](const NumberPart& p) {
    std::u16string_view t = p.type;
    return t == u"integer" || t == u"group" || t == u"decimal" || t == u"fraction" || t == u"exponentSeparator" ||
           t == u"exponentMinusSign" || t == u"exponentInteger" || t == u"compact" || t == u"nan" || t == u"infinity";
  };
  auto split3 = [&](const std::vector<NumberPart>& ps, size_t& begin, size_t& end) {
    begin = 0;
    while (begin < ps.size() && !isNumberPart(ps[begin])) begin++;
    end = ps.size();
    while (end > begin && !isNumberPart(ps[end - 1])) end--;
  };
  size_t xb, xe, yb, ye;
  split3(xs, xb, xe);
  split3(ys, yb, ye);
  auto affix = [](const std::vector<NumberPart>& ps, size_t a, size_t b) {
    std::u16string s;
    for (size_t i = a; i < b; i++) s += ps[i].value;
    return s;
  };
  std::u16string xPrefix = affix(xs, 0, xb), xSuffix = affix(xs, xe, xs.size());
  std::u16string yPrefix = affix(ys, 0, yb), ySuffix = affix(ys, ye, ys.size());
  size_t affixLength = xPrefix.size() + xSuffix.size();
  bool collapse = xPrefix == yPrefix && xSuffix == ySuffix && affixLength > 1;
  std::u16string separator(patternField(nf.dataLocale, 8));
  if (!collapse && affixLength > 0) {
    if (separator.front() != u' ') separator = u" " + separator;
    if (separator.back() != u' ') separator += u" ";
  }
  if (collapse) {
    for (size_t i = 0; i < xb; i++) out.push_back(xs[i]), sources.push_back(u"shared");
    for (size_t i = xb; i < xe; i++) out.push_back(xs[i]), sources.push_back(u"startRange");
    out.push_back({u"literal", separator});
    sources.push_back(u"shared");
    for (size_t i = yb; i < ye; i++) out.push_back(ys[i]), sources.push_back(u"endRange");
    for (size_t i = xe; i < xs.size(); i++) out.push_back(xs[i]), sources.push_back(u"shared");
  } else {
    for (const NumberPart& p : xs) out.push_back(p), sources.push_back(u"startRange");
    out.push_back({u"literal", separator});
    sources.push_back(u"shared");
    for (const NumberPart& p : ys) out.push_back(p), sources.push_back(u"endRange");
  }
  return std::make_pair(out, sources);
}

// UnwrapNumberFormat ( nf ) then the [[InitializedNumberFormat]] check.
Result<NumberFormatObject*> unwrapNumberFormat(Value nf, const char* method) {
  if (!nf.isObject()) return ThrowTypeError(std::string(method) + " called on incompatible receiver");
  Object* o = nf.asObject();
  if (!o->is<NumberFormatObject>()) {
    bool instance = TRY(OrdinaryHasInstance(builtins::intrinsic(Intrinsic::IntlNumberFormat), nf));
    if (instance) {
      Value v = TRY(Get(o, PropertyKey(fallbackSymbol())));
      if (v.isObject() && v.asObject()->is<NumberFormatObject>()) return v.asObject()->as<NumberFormatObject>();
    }
    return ThrowTypeError(std::string(method) + " called on incompatible receiver");
  }
  return o->as<NumberFormatObject>();
}

// 15.1.1 Intl.NumberFormat ( [ locales [ , options ] ] )
Result<Value> NumberFormat_construct(NativeCall& call) {
  Object* newTarget = builtins::newTargetOrCallee(call);
  NumberFormatObject* nf = TRY(OrdinaryCreateFromConstructor<NumberFormatObject>(newTarget, Intrinsic::IntlNumberFormatPrototype));
  TRY(InitializeNumberFormat(nf, call.args[0], call.args[1]));
  // ChainNumberFormat ( numberFormat, newTarget, this )
  if (!call.newTarget && call.thisValue.isObject()) {
    bool instance = TRY(OrdinaryHasInstance(builtins::intrinsic(Intrinsic::IntlNumberFormat), call.thisValue));
    if (instance) {
      TRY(DefinePropertyOrThrow(call.thisValue.asObject(), PropertyKey(fallbackSymbol()),
                                PropertyDescriptor::data(Value(nf), Attr::None)));
      return call.thisValue;
    }
  }
  return Value(nf);
}

Result<Value> NumberFormat_supportedLocalesOf(NativeCall& call) { return supportedLocalesOf(kNumberFormatData, call); }

Result<Value> boundFormat(NativeCall& call) {
  NumberFormatObject* nf = call.callee->slots[0].asObject()->as<NumberFormatObject>();
  Decimal x = TRY(ToIntlMathematicalValue(call.args[0]));
  return Value(newString(FormatNumeric(*nf, x)));
}

// 15.3.3 get Intl.NumberFormat.prototype.format
Result<Value> NumberFormatPrototype_format(NativeCall& call) {
  NumberFormatObject* nf = TRY(unwrapNumberFormat(call.thisValue, "get Intl.NumberFormat.prototype.format"));
  if (!nf->boundFormat) {
    BuiltinFunction* f = createBuiltinFunction(*builtins::currentRealm(), boundFormat, 1, PropertyKey(u""));
    f->slots.push_back(Value(nf));
    nf->boundFormat = f;
  }
  return Value(nf->boundFormat);
}

Result<Value> NumberFormatPrototype_formatToParts(NativeCall& call) {
  NumberFormatObject* nf = TRY(thisIntlObject<NumberFormatObject>(call.thisValue, "Intl.NumberFormat.prototype.formatToParts"));
  Decimal x = TRY(ToIntlMathematicalValue(call.args[0]));
  return Value(partsToArray(PartitionNumberPattern(*nf, x)));
}

Result<std::pair<Decimal, Decimal>> rangeArguments(NativeCall& call) {
  if (call.args[0].isUndefined() || call.args[1].isUndefined()) return ThrowTypeError("Intl.NumberFormat: formatRange needs start and end");
  Decimal x = TRY(ToIntlMathematicalValue(call.args[0]));
  Decimal y = TRY(ToIntlMathematicalValue(call.args[1]));
  return std::make_pair(x, y);
}

Result<Value> NumberFormatPrototype_formatRange(NativeCall& call) {
  NumberFormatObject* nf = TRY(thisIntlObject<NumberFormatObject>(call.thisValue, "Intl.NumberFormat.prototype.formatRange"));
  auto [x, y] = TRY(rangeArguments(call));
  auto [parts, sources] = TRY(partitionRange(*nf, x, y));
  std::u16string out;
  for (const NumberPart& p : parts) out += p.value;
  return Value(newString(out));
}

Result<Value> NumberFormatPrototype_formatRangeToParts(NativeCall& call) {
  NumberFormatObject* nf = TRY(thisIntlObject<NumberFormatObject>(call.thisValue, "Intl.NumberFormat.prototype.formatRangeToParts"));
  auto [x, y] = TRY(rangeArguments(call));
  auto [parts, sources] = TRY(partitionRange(*nf, x, y));
  return Value(partsToArray(parts, &sources));
}

// 15.3.5 Intl.NumberFormat.prototype.resolvedOptions ( )
Result<Value> NumberFormatPrototype_resolvedOptions(NativeCall& call) {
  NumberFormatObject* nf = TRY(unwrapNumberFormat(call.thisValue, "Intl.NumberFormat.prototype.resolvedOptions"));
  Object* o = builtins::newPlainObject();
  addProperty(o, u"locale", jsString(nf->locale));
  addProperty(o, u"numberingSystem", jsString(nf->numberingSystem));
  addProperty(o, u"style", jsString(nf->style));
  if (nf->style == "currency") {
    addProperty(o, u"currency", jsString(nf->currency));
    addProperty(o, u"currencyDisplay", jsString(nf->currencyDisplay));
    addProperty(o, u"currencySign", jsString(nf->currencySign));
  }
  if (nf->style == "unit") {
    addProperty(o, u"unit", jsString(nf->unit));
    addProperty(o, u"unitDisplay", jsString(nf->unitDisplay));
  }
  const DigitOptions& d = nf->digits;
  addProperty(o, u"minimumIntegerDigits", num(d.minimumIntegerDigits));
  if (d.hasFractionDigits) {
    addProperty(o, u"minimumFractionDigits", num(d.minimumFractionDigits));
    addProperty(o, u"maximumFractionDigits", num(d.maximumFractionDigits));
  }
  if (d.hasSignificantDigits) {
    addProperty(o, u"minimumSignificantDigits", num(d.minimumSignificantDigits));
    addProperty(o, u"maximumSignificantDigits", num(d.maximumSignificantDigits));
  }
  addProperty(o, u"useGrouping", nf->useGrouping == "false" ? Value::boolean(false) : jsString(nf->useGrouping));
  addProperty(o, u"notation", jsString(nf->notation));
  if (nf->notation == "compact") addProperty(o, u"compactDisplay", jsString(nf->compactDisplay));
  addProperty(o, u"signDisplay", jsString(nf->signDisplay));
  addProperty(o, u"roundingIncrement", num(d.roundingIncrement));
  addProperty(o, u"roundingMode", jsString(d.roundingMode));
  addProperty(o, u"roundingPriority", jsString(d.computedRoundingPriority));
  addProperty(o, u"trailingZeroDisplay", jsString(d.trailingZeroDisplay));
  return Value(o);
}

// GetBooleanOrStringNumberFormatOption ( options, property, stringValues, fallback )
Result<std::string> getUseGrouping(Object* options, const std::string& fallback) {
  Value value = TRY(Get(options, PropertyKey(u"useGrouping")));
  if (value.isUndefined()) return fallback;
  if (value.isBoolean() && value.asBoolean()) return std::string("always");
  if (!ToBoolean(value)) return std::string("false");
  String* s = TRY(ToString(value));
  std::u16string_view v = s->view();
  if (v == u"true" || v == u"false") return fallback;
  if (v == u"min2" || v == u"auto" || v == u"always") return std::string(v.begin(), v.end());
  return ThrowRangeError("Intl.NumberFormat: invalid useGrouping " + s->toUtf8());
}

}  // namespace

// 15.1.2 InitializeNumberFormat ( numberFormat, locales, options )
Result<void> InitializeNumberFormat(NumberFormatObject* nf, Value locales, Value optionsValue) {
  std::vector<std::string> requested = TRY(CanonicalizeLocaleList(locales));
  Object* options = TRY(CoerceOptionsToObject(optionsValue));
  LocaleOptions opt;
  opt.localeMatcher = TRY(GetLocaleMatcher(options));
  std::optional<std::string> numberingSystem = TRY(GetStringOption(options, u"numberingSystem", {}));
  if (numberingSystem && !isUnicodeLocaleType(*numberingSystem))
    return ThrowRangeError("Intl.NumberFormat: invalid numberingSystem " + *numberingSystem);
  if (numberingSystem) opt.keys.emplace_back("nu", LocaleOption::string(*numberingSystem));
  ResolvedLocale r = ResolveLocale(kNumberFormatData, requested, opt, kRelevantKeys);
  nf->locale = r.locale;
  nf->dataLocale = r.dataLocale;
  nf->numberingSystem = r.get("nu");

  // SetNumberFormatUnitOptions ( intlObj, options )
  nf->style = *TRY(GetStringOption(options, u"style", {"decimal", "percent", "currency", "unit"}, "decimal"));
  std::optional<std::string> currency = TRY(GetStringOption(options, u"currency", {}));
  if (!currency) {
    if (nf->style == "currency") return ThrowTypeError("Intl.NumberFormat: currency style needs a currency");
  } else if (!isWellFormedCurrencyCode(*currency)) {
    return ThrowRangeError("Intl.NumberFormat: invalid currency code " + *currency);
  }
  std::string currencyDisplay = *TRY(GetStringOption(options, u"currencyDisplay", {"code", "symbol", "narrowSymbol", "name"}, "symbol"));
  std::string currencySign = *TRY(GetStringOption(options, u"currencySign", {"standard", "accounting"}, "standard"));
  std::optional<std::string> unit = TRY(GetStringOption(options, u"unit", {}));
  if (!unit) {
    if (nf->style == "unit") return ThrowTypeError("Intl.NumberFormat: unit style needs a unit");
  } else if (!isWellFormedUnitIdentifier(*unit)) {
    return ThrowRangeError("Intl.NumberFormat: invalid unit " + *unit);
  }
  std::string unitDisplay = *TRY(GetStringOption(options, u"unitDisplay", {"short", "narrow", "long"}, "short"));
  if (nf->style == "currency") {
    nf->currency = asciiUpper(*currency);
    nf->currencyDisplay = currencyDisplay;
    nf->currencySign = currencySign;
  }
  if (nf->style == "unit") {
    nf->unit = *unit;
    nf->unitDisplay = unitDisplay;
  }

  nf->notation = *TRY(GetStringOption(options, u"notation", {"standard", "scientific", "engineering", "compact"}, "standard"));
  int mnfdDefault, mxfdDefault;
  if (nf->style == "currency" && nf->notation == "standard") {
    mnfdDefault = mxfdDefault = currencyDigits(nf->currency);
  } else {
    mnfdDefault = 0;
    mxfdDefault = nf->style == "percent" ? 0 : 3;
  }
  TRY(SetNumberFormatDigitOptions(nf->digits, options, mnfdDefault, mxfdDefault, nf->notation));
  std::string compactDisplay = *TRY(GetStringOption(options, u"compactDisplay", {"short", "long"}, "short"));
  std::string defaultUseGrouping = "auto";
  if (nf->notation == "compact") {
    nf->compactDisplay = compactDisplay;
    defaultUseGrouping = "min2";
  }
  nf->useGrouping = TRY(getUseGrouping(options, defaultUseGrouping));
  nf->signDisplay = *TRY(GetStringOption(options, u"signDisplay", {"auto", "never", "always", "exceptZero", "negative"}, "auto"));
  return {};
}

Result<NumberFormatObject*> createNumberFormat(Value locales, Value options) {
  NumberFormatObject* nf = make<NumberFormatObject>(builtins::intrinsic(Intrinsic::IntlNumberFormatPrototype));
  TRY(InitializeNumberFormat(nf, locales, options));
  return nf;
}

void installNumberFormat(Realm& realm, Object* intl) {
  Object* proto = nullptr;
  BuiltinFunction* ctor = defineIntlConstructor(realm, intl, Intrinsic::IntlNumberFormat, NumberFormat_construct, 0,
                                                u"NumberFormat", Intrinsic::IntlNumberFormatPrototype, &proto);
  defineFunction(realm, ctor, PropertyKey(u"supportedLocalesOf"), 1, NumberFormat_supportedLocalesOf);
  defineAccessor(realm, proto, PropertyKey(u"format"), NumberFormatPrototype_format, nullptr);
  defineFunction(realm, proto, PropertyKey(u"formatRange"), 2, NumberFormatPrototype_formatRange);
  defineFunction(realm, proto, PropertyKey(u"formatRangeToParts"), 2, NumberFormatPrototype_formatRangeToParts);
  defineFunction(realm, proto, PropertyKey(u"formatToParts"), 1, NumberFormatPrototype_formatToParts);
  defineFunction(realm, proto, PropertyKey(u"resolvedOptions"), 0, NumberFormatPrototype_resolvedOptions);
}

}  // namespace snowman::intl
