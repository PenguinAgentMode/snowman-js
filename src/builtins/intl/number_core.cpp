// Intl mathematical values, rounding (ECMA-402 15.5) and plural rules.
#include "builtins/intl/number_core.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>

#include "runtime/bigint.h"

namespace snowman::intl {

namespace {

// ---- decimal digit strings (no sign) ----

std::string stripLeadingZeros(std::string s) {
  size_t i = 0;
  while (i < s.size() && s[i] == '0') i++;
  return s.substr(i);
}

// (q, r) = divmod(n, k) for a small k.
std::pair<std::string, int> divmodSmall(const std::string& n, int k) {
  std::string q;
  int r = 0;
  for (char c : n) {
    r = r * 10 + (c - '0');
    q.push_back(char('0' + r / k));
    r %= k;
  }
  return {stripLeadingZeros(q), r};
}

std::string mulSmall(const std::string& n, int k) {
  if (n.empty() || k == 0) return {};
  std::string out(n.size() + 6, '0');
  int carry = 0;
  size_t pos = out.size();
  for (size_t i = n.size(); i-- > 0;) {
    int v = (n[i] - '0') * k + carry;
    out[--pos] = char('0' + v % 10);
    carry = v / 10;
  }
  while (carry) {
    out[--pos] = char('0' + carry % 10);
    carry /= 10;
  }
  return stripLeadingZeros(out);
}

std::string addOne(std::string n) {
  for (size_t i = n.size(); i-- > 0;) {
    if (n[i] != '9') {
      n[i]++;
      return n;
    }
    n[i] = '0';
  }
  return "1" + n;
}

enum class UnsignedRounding : uint8_t { Zero, Infinity, HalfZero, HalfInfinity, HalfEven };

// 15.5.15 GetUnsignedRoundingMode ( roundingMode, sign )
UnsignedRounding unsignedRoundingMode(std::string_view mode, bool negative) {
  if (mode == "ceil") return negative ? UnsignedRounding::Zero : UnsignedRounding::Infinity;
  if (mode == "floor") return negative ? UnsignedRounding::Infinity : UnsignedRounding::Zero;
  if (mode == "expand") return UnsignedRounding::Infinity;
  if (mode == "trunc") return UnsignedRounding::Zero;
  if (mode == "halfCeil") return negative ? UnsignedRounding::HalfZero : UnsignedRounding::HalfInfinity;
  if (mode == "halfFloor") return negative ? UnsignedRounding::HalfInfinity : UnsignedRounding::HalfZero;
  if (mode == "halfTrunc") return UnsignedRounding::HalfZero;
  if (mode == "halfEven") return UnsignedRounding::HalfEven;
  return UnsignedRounding::HalfInfinity;  // halfExpand
}

// x (finite, non-negative) rounded to a multiple of increment × 10^p.
Decimal roundToIncrement(const Decimal& x, int64_t p, int increment, UnsignedRounding mode) {
  while (increment % 10 == 0) {
    increment /= 10;
    p++;
  }
  Decimal out;
  if (x.digits.empty()) return out;
  // x / 10^p = N + rest, N an integer, 0 <= rest < 1.
  std::string n, rest;
  if (x.exponent >= p) {
    n = x.digits + std::string(size_t(x.exponent - p), '0');
  } else {
    int64_t drop = p - x.exponent;
    if (drop >= int64_t(x.digits.size())) {
      rest = std::string(size_t(drop - int64_t(x.digits.size())), '0') + x.digits;
    } else {
      n = x.digits.substr(0, x.digits.size() - size_t(drop));
      rest = x.digits.substr(x.digits.size() - size_t(drop));
    }
  }
  bool restZero = rest.find_first_not_of('0') == std::string::npos;
  // rest compared with 1/2.
  int restHalf = -1;
  if (!rest.empty()) {
    if (rest[0] > '5') restHalf = 1;
    else if (rest[0] == '5') restHalf = rest.find_first_not_of('0', 1) == std::string::npos ? 0 : 1;
  }
  auto [q, r] = divmodSmall(n, increment);
  // frac = (r + rest) / increment, compared with 0 and 1/2.
  bool fracZero = r == 0 && restZero;
  int cmpHalf;
  if (restZero) cmpHalf = (2 * r > increment) - (2 * r < increment);
  else if (restHalf < 0) cmpHalf = 2 * r >= increment ? 1 : -1;
  else if (restHalf == 0) cmpHalf = (2 * r + 1 > increment) - (2 * r + 1 < increment);
  else cmpHalf = 2 * r + 1 >= increment ? 1 : -1;
  bool up = false;
  switch (mode) {
    case UnsignedRounding::Zero: up = false; break;
    case UnsignedRounding::Infinity: up = !fracZero; break;
    case UnsignedRounding::HalfZero: up = cmpHalf > 0; break;
    case UnsignedRounding::HalfInfinity: up = cmpHalf >= 0 && !fracZero; break;
    case UnsignedRounding::HalfEven:
      up = cmpHalf > 0 || (cmpHalf == 0 && !q.empty() && ((q.back() - '0') % 2 == 1));
      break;
  }
  if (fracZero) up = false;
  if (up) q = q.empty() ? "1" : addOne(q);
  out.digits = mulSmall(q, increment);
  out.exponent = p;
  out.normalize();
  return out;
}

// The digits of a non-negative finite value as an integer times 10^-f (f >= 0 fraction digits), as a string.
std::string integerTimesPow10(const Decimal& x, int64_t f) {
  if (x.digits.empty()) return "0";
  int64_t e = x.exponent + f;  // x × 10^f = digits × 10^e, e >= 0 here
  return x.digits + std::string(size_t(std::max<int64_t>(e, 0)), '0');
}

struct RawResult {
  Decimal rounded;
  std::string formatted;  // "123.45"
  int64_t integerDigits;
  int64_t roundingMagnitude;
};

// 15.5.8 ToRawPrecision ( x, minPrecision, maxPrecision, unsignedRoundingMode )
RawResult ToRawPrecision(const Decimal& x, int minPrecision, int maxPrecision, UnsignedRounding mode) {
  int p = maxPrecision;
  RawResult r;
  std::string m;
  int64_t e;
  if (x.isZero()) {
    m = std::string(size_t(p), '0');
    e = 0;
  } else {
    e = x.magnitude();
    r.rounded = roundToIncrement(x, e - p + 1, 1, mode);
    if (!r.rounded.digits.empty() && r.rounded.magnitude() > e) e = r.rounded.magnitude();
    // The p significant digits.
    m = r.rounded.digits;
    int64_t pos = r.rounded.exponent - (e - p + 1);
    m += std::string(size_t(std::max<int64_t>(pos, 0)), '0');
    if (int64_t(m.size()) < p) m += std::string(size_t(p) - m.size(), '0');
  }
  r.roundingMagnitude = e - p + 1;
  if (e >= p - 1) {
    m += std::string(size_t(e - p + 1), '0');
    r.integerDigits = e + 1;
  } else if (e >= 0) {
    m = m.substr(0, size_t(e + 1)) + "." + m.substr(size_t(e + 1));
    r.integerDigits = e + 1;
  } else {
    m = "0." + std::string(size_t(-(e + 1)), '0') + m;
    r.integerDigits = 1;
  }
  if (m.find('.') != std::string::npos && maxPrecision > minPrecision) {
    int cut = maxPrecision - minPrecision;
    while (cut > 0 && m.back() == '0') {
      m.pop_back();
      cut--;
    }
    if (m.back() == '.') m.pop_back();
  }
  r.formatted = m;
  return r;
}

// 15.5.9 ToRawFixed ( x, minFraction, maxFraction, roundingIncrement, unsignedRoundingMode )
RawResult ToRawFixed(const Decimal& x, int minFraction, int maxFraction, int roundingIncrement, UnsignedRounding mode) {
  int f = maxFraction;
  RawResult r;
  r.rounded = roundToIncrement(x, -f, roundingIncrement, mode);
  std::string m = integerTimesPow10(r.rounded, f);
  if (f != 0) {
    size_t k = m.size();
    if (k <= size_t(f)) {
      m = std::string(size_t(f) + 1 - k, '0') + m;
      k = size_t(f) + 1;
    }
    std::string a = m.substr(0, k - size_t(f)), b = m.substr(k - size_t(f));
    m = a + "." + b;
    r.integerDigits = int64_t(a.size());
  } else {
    r.integerDigits = int64_t(m.size());
  }
  int cut = maxFraction - minFraction;
  while (cut > 0 && m.back() == '0') {
    m.pop_back();
    cut--;
  }
  if (m.back() == '.') m.pop_back();
  r.formatted = m;
  r.roundingMagnitude = -f;
  return r;
}

bool isStrWhiteSpace(char16_t c) { return isWhiteSpaceOrLineTerminator(c); }

}  // namespace

// ---- Decimal ----

void Decimal::normalize() {
  size_t lead = digits.find_first_not_of('0');
  if (lead == std::string::npos) {
    digits.clear();
    exponent = 0;
    return;
  }
  digits.erase(0, lead);
  size_t trail = digits.find_last_not_of('0');
  exponent += int64_t(digits.size() - 1 - trail);
  digits.resize(trail + 1);
}

Decimal Decimal::fromDouble(double d) {
  Decimal x;
  if (std::isnan(d)) return nan();
  if (std::isinf(d)) {
    x.kind = d > 0 ? Kind::PositiveInfinity : Kind::NegativeInfinity;
    return x;
  }
  x.negative = std::signbit(d);
  if (d == 0) return x;
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof buf, std::fabs(d), std::chars_format::scientific);
  std::string_view s(buf, size_t(res.ptr - buf));
  size_t epos = s.find('e');
  std::string mant(s.substr(0, epos));
  int64_t exp10 = std::strtoll(std::string(s.substr(epos + 1)).c_str(), nullptr, 10);
  std::string digits;
  for (char c : mant)
    if (c != '.') digits.push_back(c);
  x.digits = digits;
  x.exponent = exp10 - int64_t(digits.size() - 1);
  x.normalize();
  return x;
}

Decimal Decimal::fromIntegerString(std::string_view digits, bool negative) {
  Decimal x;
  x.negative = negative;
  x.digits = std::string(digits);
  x.normalize();
  return x;
}

Decimal Decimal::fromBigInt(BigInt* b) {
  std::u16string s = b->toString(10);
  bool negative = !s.empty() && s[0] == u'-';
  std::string digits;
  for (char16_t c : s)
    if (c != u'-') digits.push_back(char(c));
  Decimal x = fromIntegerString(digits, negative);
  if (x.digits.empty()) x.negative = false;
  return x;
}

double Decimal::toDouble() const {
  switch (kind) {
    case Kind::NaN: return NAN;
    case Kind::PositiveInfinity: return INFINITY;
    case Kind::NegativeInfinity: return -INFINITY;
    case Kind::Finite: break;
  }
  if (digits.empty()) return negative ? -0.0 : 0.0;
  std::string s = (negative ? "-" : "") + digits + "e" + std::to_string(exponent);
  return std::strtod(s.c_str(), nullptr);
}

int compareDecimal(const Decimal& a, const Decimal& b) {
  auto rank = [](const Decimal& d) {
    if (d.kind == Decimal::Kind::NegativeInfinity) return -2;
    if (d.kind == Decimal::Kind::PositiveInfinity) return 2;
    return 0;
  };
  if (rank(a) || rank(b)) return (rank(a) > rank(b)) - (rank(a) < rank(b));
  int sa = a.digits.empty() ? 0 : (a.negative ? -1 : 1), sb = b.digits.empty() ? 0 : (b.negative ? -1 : 1);
  if (sa != sb) return (sa > sb) - (sa < sb);
  if (sa == 0) return 0;
  int mag = (a.magnitude() > b.magnitude()) - (a.magnitude() < b.magnitude());
  if (mag == 0) {
    size_t n = std::max(a.digits.size(), b.digits.size());
    std::string da = a.digits + std::string(n - a.digits.size(), '0'), db = b.digits + std::string(n - b.digits.size(), '0');
    mag = (da > db) - (da < db);
  }
  return sa > 0 ? mag : -mag;
}

Decimal parseIntlMathematicalValue(std::u16string_view text) {
  size_t a = 0, b = text.size();
  while (a < b && isStrWhiteSpace(text[a])) a++;
  while (b > a && isStrWhiteSpace(text[b - 1])) b--;
  std::u16string_view s = text.substr(a, b - a);
  Decimal x;
  if (s.empty()) return x;
  if (s.size() > 2 && s[0] == u'0') {
    int radix = 0;
    if (s[1] == u'x' || s[1] == u'X') radix = 16;
    else if (s[1] == u'o' || s[1] == u'O') radix = 8;
    else if (s[1] == u'b' || s[1] == u'B') radix = 2;
    if (radix) {
      std::string digits;
      for (char16_t c : s.substr(2)) {
        int v = c >= u'0' && c <= u'9' ? c - u'0' : c >= u'a' && c <= u'f' ? c - u'a' + 10 : c >= u'A' && c <= u'F' ? c - u'A' + 10 : 99;
        if (v >= radix) return Decimal::nan();
        digits.push_back(char(c));
      }
      std::u16string dec = BigInt::fromDigits(digits, radix)->toString(10);
      return Decimal::fromIntegerString(std::string(dec.begin(), dec.end()), false);
    }
  }
  size_t i = 0;
  if (s[0] == u'+' || s[0] == u'-') {
    x.negative = s[0] == u'-';
    i = 1;
  }
  std::u16string_view rest = s.substr(i);
  if (rest == u"Infinity") {
    x.kind = x.negative ? Decimal::Kind::NegativeInfinity : Decimal::Kind::PositiveInfinity;
    x.negative = false;
    return x;
  }
  std::string digits;
  int64_t fractionDigits = 0;
  bool sawDigit = false, sawPoint = false;
  for (; i < s.size(); i++) {
    char16_t c = s[i];
    if (c >= u'0' && c <= u'9') {
      digits.push_back(char(c));
      if (sawPoint) fractionDigits++;
      sawDigit = true;
    } else if (c == u'.' && !sawPoint) {
      sawPoint = true;
    } else {
      break;
    }
  }
  if (!sawDigit) return Decimal::nan();
  int64_t exp = 0;
  if (i < s.size()) {
    if (s[i] != u'e' && s[i] != u'E') return Decimal::nan();
    i++;
    bool expNegative = false;
    if (i < s.size() && (s[i] == u'+' || s[i] == u'-')) expNegative = s[i++] == u'-';
    if (i == s.size()) return Decimal::nan();
    for (; i < s.size(); i++) {
      if (s[i] < u'0' || s[i] > u'9') return Decimal::nan();
      if (exp < 1000000000) exp = exp * 10 + (s[i] - u'0');
    }
    if (expNegative) exp = -exp;
  }
  x.digits = digits;
  x.exponent = exp - fractionDigits;
  x.normalize();
  return x;
}

// 15.5.16 ToIntlMathematicalValue ( value )
Result<Decimal> ToIntlMathematicalValue(Value value) {
  if (value.isNumber()) return Decimal::fromDouble(value.asNumber());
  Value prim = TRY(ToPrimitive(value, PreferredType::Number));
  if (prim.isBigInt()) return Decimal::fromBigInt(prim.asBigInt());
  if (!prim.isString()) {
    double d = TRY(ToNumber(prim));
    return Decimal::fromDouble(d);
  }
  Decimal x = parseIntlMathematicalValue(prim.asString()->view());
  if (x.isFinite() && !x.digits.empty()) {
    // RoundMVResult: magnitudes a Number cannot hold become infinities or zero.
    double d = x.abs().toDouble();
    if (std::isinf(d)) {
      bool negative = x.negative;
      x = Decimal();
      x.kind = negative ? Decimal::Kind::NegativeInfinity : Decimal::Kind::PositiveInfinity;
    } else if (d == 0) {
      bool negative = x.negative;
      x = Decimal();
      x.negative = negative;
    }
  }
  return x;
}

// ---- digit options ----

Result<void> SetNumberFormatDigitOptions(DigitOptions& d, Object* options, int mnfdDefault, int mxfdDefault,
                                         std::string_view notation) {
  d.minimumIntegerDigits = int(*TRY(GetNumberOption(options, u"minimumIntegerDigits", 1, 21, 1)));
  Value mnfd = TRY(Get(options, PropertyKey(u"minimumFractionDigits")));
  Value mxfd = TRY(Get(options, PropertyKey(u"maximumFractionDigits")));
  Value mnsd = TRY(Get(options, PropertyKey(u"minimumSignificantDigits")));
  Value mxsd = TRY(Get(options, PropertyKey(u"maximumSignificantDigits")));
  int roundingIncrement = int(*TRY(GetNumberOption(options, u"roundingIncrement", 1, 5000, 1)));
  static const int increments[] = {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000};
  if (std::find(std::begin(increments), std::end(increments), roundingIncrement) == std::end(increments))
    return ThrowRangeError("Intl: invalid roundingIncrement");
  d.roundingMode = *TRY(GetStringOption(options, u"roundingMode",
                                        {"ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc", "halfEven"},
                                        "halfExpand"));
  std::string roundingPriority = *TRY(GetStringOption(options, u"roundingPriority", {"auto", "morePrecision", "lessPrecision"}, "auto"));
  d.trailingZeroDisplay = *TRY(GetStringOption(options, u"trailingZeroDisplay", {"auto", "stripIfInteger"}, "auto"));
  if (roundingIncrement != 1) mxfdDefault = mnfdDefault;
  d.roundingIncrement = roundingIncrement;
  bool hasSd = !mnsd.isUndefined() || !mxsd.isUndefined();
  bool hasFd = !mnfd.isUndefined() || !mxfd.isUndefined();
  bool needSd = true, needFd = true;
  if (roundingPriority == "auto") {
    needSd = hasSd;
    if (needSd || (!hasFd && notation == "compact")) needFd = false;
  }
  d.hasSignificantDigits = needSd;
  d.hasFractionDigits = needFd;
  if (needSd) {
    if (hasSd) {
      double mn = *TRY(DefaultNumberOption(mnsd, 1, 21, 1));
      double mx = *TRY(DefaultNumberOption(mxsd, mn, 21, 21));
      d.minimumSignificantDigits = int(mn);
      d.maximumSignificantDigits = int(mx);
    } else {
      d.minimumSignificantDigits = 1;
      d.maximumSignificantDigits = 21;
    }
  }
  if (needFd) {
    if (hasFd) {
      std::optional<double> mn = TRY(DefaultNumberOption(mnfd, 0, 100, std::nullopt));
      std::optional<double> mx = TRY(DefaultNumberOption(mxfd, 0, 100, std::nullopt));
      if (!mn) mn = std::min<double>(mnfdDefault, *mx);
      else if (!mx) mx = std::max<double>(mxfdDefault, *mn);
      else if (*mn > *mx) return ThrowRangeError("Intl: minimumFractionDigits is greater than maximumFractionDigits");
      d.minimumFractionDigits = int(*mn);
      d.maximumFractionDigits = int(*mx);
    } else {
      d.minimumFractionDigits = mnfdDefault;
      d.maximumFractionDigits = mxfdDefault;
    }
  }
  if (!needSd && !needFd) {
    d.minimumFractionDigits = 0;
    d.maximumFractionDigits = 0;
    d.minimumSignificantDigits = 1;
    d.maximumSignificantDigits = 2;
    d.hasFractionDigits = d.hasSignificantDigits = true;
    d.roundingType = RoundingType::MorePrecision;
    d.computedRoundingPriority = "morePrecision";
  } else if (roundingPriority == "auto") {
    d.roundingType = needSd ? RoundingType::SignificantDigits : RoundingType::FractionDigits;
    d.computedRoundingPriority = "auto";
  } else {
    d.roundingType = roundingPriority == "morePrecision" ? RoundingType::MorePrecision : RoundingType::LessPrecision;
    d.computedRoundingPriority = roundingPriority;
  }
  if (roundingIncrement != 1) {
    if (d.roundingType != RoundingType::FractionDigits) return ThrowTypeError("Intl: roundingIncrement needs fraction digit rounding");
    if (d.maximumFractionDigits != d.minimumFractionDigits)
      return ThrowRangeError("Intl: roundingIncrement needs equal minimum and maximum fraction digits");
  }
  return {};
}

// 15.5.3 FormatNumericToString ( intlObject, x )
RawFormat FormatNumericToString(const DigitOptions& d, const Decimal& input) {
  bool negative = input.negative;
  Decimal x = input.abs();
  UnsignedRounding mode = unsignedRoundingMode(d.roundingMode, negative);
  RawResult result;
  switch (d.roundingType) {
    case RoundingType::SignificantDigits:
      result = ToRawPrecision(x, d.minimumSignificantDigits, d.maximumSignificantDigits, mode);
      break;
    case RoundingType::FractionDigits:
      result = ToRawFixed(x, d.minimumFractionDigits, d.maximumFractionDigits, d.roundingIncrement, mode);
      break;
    default: {
      RawResult s = ToRawPrecision(x, d.minimumSignificantDigits, d.maximumSignificantDigits, mode);
      RawResult f = ToRawFixed(x, d.minimumFractionDigits, d.maximumFractionDigits, d.roundingIncrement, mode);
      bool fixedIsMorePrecise;
      if (d.roundingType == RoundingType::MorePrecision) fixedIsMorePrecise = !(s.roundingMagnitude <= f.roundingMagnitude);
      else fixedIsMorePrecise = s.roundingMagnitude <= f.roundingMagnitude;
      result = fixedIsMorePrecise ? f : s;
      break;
    }
  }
  std::string string = result.formatted;
  bool isInteger = result.rounded.digits.empty() || result.rounded.exponent >= 0;
  if (d.trailingZeroDisplay == "stripIfInteger" && isInteger) {
    size_t dot = string.find('.');
    if (dot != std::string::npos) string.resize(dot);
  }
  RawFormat out;
  size_t dot = string.find('.');
  out.integer = string.substr(0, dot);
  if (dot != std::string::npos) out.fraction = string.substr(dot + 1);
  if (int64_t(out.integer.size()) < d.minimumIntegerDigits)
    out.integer = std::string(size_t(d.minimumIntegerDigits) - out.integer.size(), '0') + out.integer;
  out.rounded = result.rounded;
  out.rounded.negative = negative;
  return out;
}

// ---- plural rules ----

namespace {

struct AliasEntry {
  const char* from;
  const char* to;
};
#include "builtins/intl/plural_data.inc"

// The operands of a formatted number (UTS 35 plural operands); n, i are kept as decimal strings for exact modulo.
struct Operands {
  std::string integer;   // i
  std::string fraction;  // the visible fraction digits
  int exponent = 0;      // c / e
};

// x mod m for a non-negative decimal integer string.
long double modString(const std::string& digits, long double m) {
  long double r = 0;
  for (char c : digits) r = std::fmod(r * 10 + (c - '0'), m);
  return r;
}

class RuleParser {
 public:
  RuleParser(std::string_view s, const Operands& ops) : s_(s), ops_(ops) {}

  bool condition() {
    bool result = andCondition();
    while (keyword("or")) result = andCondition() || result;
    return result;
  }

 private:
  void skip() {
    while (pos_ < s_.size() && s_[pos_] == ' ') pos_++;
  }
  bool keyword(std::string_view k) {
    skip();
    if (s_.substr(pos_, k.size()) == k) {
      pos_ += k.size();
      return true;
    }
    return false;
  }
  long double number() {
    skip();
    long double v = 0;
    while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') v = v * 10 + (s_[pos_++] - '0');
    return v;
  }
  bool andCondition() {
    bool result = relation();
    while (keyword("and")) result = relation() && result;
    return result;
  }
  long double operand(char op, std::optional<long double> mod) {
    std::string trimmed = ops_.fraction;
    while (!trimmed.empty() && trimmed.back() == '0') trimmed.pop_back();
    auto withMod = [&](const std::string& digits, long double frac) -> long double {
      if (mod) return modString(digits.empty() ? "0" : digits, *mod) + frac;
      return std::strtold((digits.empty() ? "0" : digits).c_str(), nullptr) + frac;
    };
    switch (op) {
      case 'n': {
        long double frac = ops_.fraction.empty() ? 0 : std::strtold(("0." + ops_.fraction).c_str(), nullptr);
        return withMod(ops_.integer, frac);
      }
      case 'i': return withMod(ops_.integer, 0);
      case 'v': return withMod(std::to_string(ops_.fraction.size()), 0);
      case 'w': return withMod(std::to_string(trimmed.size()), 0);
      case 'f': return withMod(ops_.fraction, 0);
      case 't': return withMod(trimmed, 0);
      case 'c':
      case 'e': return withMod(std::to_string(ops_.exponent), 0);
      default: return 0;
    }
  }
  bool relation() {
    skip();
    char op = pos_ < s_.size() ? s_[pos_++] : 'n';
    std::optional<long double> mod;
    skip();
    if (pos_ < s_.size() && s_[pos_] == '%') {
      pos_++;
      mod = number();
    }
    long double value = operand(op, mod);
    bool negate = false;
    if (keyword("!=")) negate = true;
    else keyword("=");
    bool match = false;
    do {
      long double lo = number(), hi = lo;
      if (keyword("..")) hi = number();
      bool isInteger = value == std::floor(value);
      if (lo == hi ? value == lo : (isInteger && value >= lo && value <= hi)) match = true;
    } while (keyword(","));
    return negate ? !match : match;
  }

  std::string_view s_;
  const Operands& ops_;
  size_t pos_ = 0;
};

PluralRuleSet buildRuleSet(std::string_view locale, bool ordinal) {
  PluralRuleSet set;
  std::string candidate(locale);
  auto find = [](const AliasEntry* begin, const AliasEntry* end, const std::string& key) -> const char* {
    const AliasEntry* it = std::lower_bound(begin, end, key, [](const AliasEntry& e, const std::string& k) { return std::string_view(e.from) < k; });
    return it != end && key == it->from ? it->to : nullptr;
  };
  const char* rules = nullptr;
  const char* ranges = nullptr;
  for (const std::string& c : data::lookupChain(candidate)) {
    if (!rules) rules = ordinal ? find(std::begin(kOrdinalRules), std::end(kOrdinalRules), c) : find(std::begin(kCardinalRules), std::end(kCardinalRules), c);
    if (!ranges) ranges = find(std::begin(kPluralRanges), std::end(kPluralRanges), c);
  }
  if (rules && *rules) {
    for (const std::string& part : split(rules, '\x1d')) set.rules.emplace_back(part[0], part.substr(1));
  }
  if (ranges) set.ranges = ranges;
  return set;
}

}  // namespace

const PluralRuleSet& pluralRulesFor(std::string_view locale, bool ordinal) {
  thread_local std::vector<std::pair<std::string, PluralRuleSet>> cache[2];
  auto& c = cache[ordinal ? 1 : 0];
  for (const auto& [l, set] : c)
    if (l == locale) return set;
  c.emplace_back(std::string(locale), buildRuleSet(locale, ordinal));
  return c.back().second;
}

char pluralCategory(const PluralRuleSet& rules, const std::string& integer, const std::string& fraction, int exponent) {
  // A compact exponent moves fraction digits into the integer: 1.2 with c=3 has i = 1200, v = 0.
  std::string i = integer, f = fraction;
  if (exponent > 0) {
    std::string padded = f + std::string(size_t(exponent) > f.size() ? size_t(exponent) - f.size() : 0, '0');
    i += padded.substr(0, size_t(exponent));
    f = padded.substr(size_t(exponent));
    while (!f.empty() && f.back() == '0' && fraction.size() <= size_t(exponent)) f.pop_back();
  }
  Operands ops{i, f, exponent};
  for (const auto& [category, condition] : rules.rules) {
    if (RuleParser(condition, ops).condition()) return category;
  }
  return 'x';
}

std::string_view categoryName(char letter) {
  switch (letter) {
    case 'z': return "zero";
    case 'o': return "one";
    case 't': return "two";
    case 'f': return "few";
    case 'm': return "many";
    default: return "other";
  }
}

char categoryLetter(std::string_view name) {
  if (name == "zero") return 'z';
  if (name == "one") return 'o';
  if (name == "two") return 't';
  if (name == "few") return 'f';
  if (name == "many") return 'm';
  return 'x';
}

std::vector<char> pluralCategories(const PluralRuleSet& rules) {
  std::vector<char> out;
  for (const auto& [c, cond] : rules.rules) out.push_back(c);
  out.push_back('x');
  return out;
}

char pluralRangeCategory(const PluralRuleSet& rules, char start, char end) {
  for (size_t i = 0; i + 2 < rules.ranges.size() + 1 && i + 3 <= rules.ranges.size(); i += 3)
    if (rules.ranges[i] == start && rules.ranges[i + 1] == end) return rules.ranges[i + 2];
  return end;
}

}  // namespace snowman::intl
