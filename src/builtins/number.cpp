// Number (21.1).
#include <bit>
#include <cmath>

#include "builtins/intl/hooks.h"
#include "builtins/util.h"
#include "runtime/bigint.h"
#include "runtime/wrappers.h"

namespace snowman::builtins {

namespace {

// 21.1.1.1 Number ( value )
Result<Value> Number_construct(NativeCall& call) {
  double n = 0;
  if (call.args.size() > 0) {
    Value prim = TRY(ToNumeric(call.args[0]));
    n = prim.isBigInt() ? prim.asBigInt()->toDouble() : prim.asNumber();
  }
  if (!call.newTarget) return num(n);
  return Value(TRY(OrdinaryCreateFromConstructor<NumberObject>(call.newTarget, Intrinsic::NumberPrototype, n)));
}

Result<Value> Number_isFinite(NativeCall& call) {
  Value v = call.args[0];
  return boolean(v.isNumber() && std::isfinite(v.asNumber()));
}

Result<Value> Number_isInteger(NativeCall& call) { return boolean(IsIntegralNumber(call.args[0])); }

Result<Value> Number_isNaN(NativeCall& call) {
  Value v = call.args[0];
  return boolean(v.isNumber() && std::isnan(v.asNumber()));
}

Result<Value> Number_isSafeInteger(NativeCall& call) {
  Value v = call.args[0];
  return boolean(IsIntegralNumber(v) && std::fabs(v.asNumber()) <= 9007199254740991.0);
}

// 21.1.3.7.1 ThisNumberValue
Result<double> thisNumberValue(Value value) {
  if (value.isNumber()) return value.asNumber();
  if (value.isObject()) {
    if (auto* n = value.asObject()->as<NumberObject>()) return n->numberData;
  }
  return ThrowTypeError("Number.prototype method called on incompatible receiver");
}

// The exact decimal expansion of a finite positive double: value = 0.digits × 10^point.
void exactDecimal(double x, std::string& digits, int& point) {
  uint64_t bits = std::bit_cast<uint64_t>(x);
  int exp = int((bits >> 52) & 0x7FF);
  uint64_t mant = bits & ((uint64_t(1) << 52) - 1);
  if (exp == 0) exp = 1;
  else mant |= uint64_t(1) << 52;
  int e = exp - 1075;
  BigInt* m = BigInt::fromUint64(mant);
  if (e >= 0) {
    m = BigInt::leftShift(m, BigInt::fromInt64(e));
    std::u16string s = m->toString();
    digits.assign(s.begin(), s.end());
    point = int(digits.size());
  } else {
    m = BigInt::multiply(m, BigInt::exponentiate(BigInt::fromInt64(5), BigInt::fromInt64(-e)));
    std::u16string s = m->toString();
    digits.assign(s.begin(), s.end());
    point = int(digits.size()) + e;
  }
  size_t firstNonZero = digits.find_first_not_of('0');
  if (firstNonZero != std::string::npos && firstNonZero > 0) {
    digits.erase(0, firstNonZero);
  }
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
}

// Round the exact expansion to `count` significant digits (ties away from zero, i.e. "pick the larger n").
// Returns the digits and the decimal exponent e of the first digit (value ≈ d.ddd × 10^e).
std::string roundSignificant(const std::string& digits, int point, int count, int& e) {
  std::string out = digits.substr(0, std::min<size_t>(digits.size(), size_t(count)));
  while (int(out.size()) < count) out.push_back('0');
  e = point - 1;
  if (int(digits.size()) > count && digits[size_t(count)] >= '5') {
    int i = count - 1;
    while (i >= 0 && out[size_t(i)] == '9') out[size_t(i--)] = '0';
    if (i < 0) {
      out.insert(out.begin(), '1');
      out.pop_back();
      e++;
    } else {
      out[size_t(i)]++;
    }
  }
  return out;
}

String* ascii(const std::string& s) { return newString(std::u16string(s.begin(), s.end())); }

// 21.1.3.3 Number.prototype.toFixed ( fractionDigits )
Result<Value> NumberPrototype_toFixed(NativeCall& call) {
  double x = TRY(thisNumberValue(call.thisValue));
  double f = TRY(ToIntegerOrInfinity(call.args[0]));
  if (!std::isfinite(f)) return ThrowRangeError("toFixed() digits argument must be between 0 and 100");
  if (f < 0 || f > 100) return ThrowRangeError("toFixed() digits argument must be between 0 and 100");
  if (!std::isfinite(x)) return Value(NumberToString(x));
  std::string s;
  if (x < 0) {
    s = "-";
    x = -x;
  }
  std::string m;
  if (x >= 1e21) {
    m = NumberToString(x)->toUtf8();
  } else {
    int fd = int(f);
    if (x == 0) {
      m = "0";
    } else {
      std::string digits;
      int point;
      exactDecimal(x, digits, point);
      // n = round(x × 10^f): the digits up to position point + f.
      int keep = point + fd;
      if (keep < 0) {
        m = "0";
      } else {
        std::string n = digits.substr(0, std::min<size_t>(digits.size(), size_t(keep)));
        while (int(n.size()) < keep) n.push_back('0');
        if (int(digits.size()) > keep && digits[size_t(keep)] >= '5') {
          int i = int(n.size()) - 1;
          while (i >= 0 && n[size_t(i)] == '9') n[size_t(i--)] = '0';
          if (i < 0) n.insert(n.begin(), '1');
          else n[size_t(i)]++;
        }
        size_t nz = n.find_first_not_of('0');
        m = nz == std::string::npos ? "0" : n.substr(nz);
      }
    }
    if (fd != 0) {
      int k = int(m.size());
      if (k <= fd) {
        m = std::string(size_t(fd + 1 - k), '0') + m;
        k = fd + 1;
      }
      m = m.substr(0, size_t(k - fd)) + "." + m.substr(size_t(k - fd));
    }
  }
  return Value(ascii(s + m));
}

std::string exponentSuffix(int e) {
  std::string out = "e";
  out.push_back(e < 0 ? '-' : '+');
  out += std::to_string(e < 0 ? -e : e);
  return out;
}

// 21.1.3.2 Number.prototype.toExponential ( fractionDigits )
Result<Value> NumberPrototype_toExponential(NativeCall& call) {
  double x = TRY(thisNumberValue(call.thisValue));
  double f = TRY(ToIntegerOrInfinity(call.args[0]));
  if (!std::isfinite(x)) return Value(NumberToString(x));
  if (f < 0 || f > 100) return ThrowRangeError("toExponential() argument must be between 0 and 100");
  std::string s;
  if (x < 0) {
    s = "-";
    x = -x;
  }
  std::string m;
  int e = 0;
  int fd = int(f);
  if (x == 0) {
    m = std::string(size_t(fd + 1), '0');
  } else if (!call.args[0].isUndefined()) {
    std::string digits;
    int point;
    exactDecimal(x, digits, point);
    m = roundSignificant(digits, point, fd + 1, e);
  } else {
    // As many digits as necessary to represent x uniquely: Number::toString's digits.
    std::string shortest = NumberToString(x)->toUtf8();
    std::string digits;
    int point = 0;
    size_t epos = shortest.find('e');
    std::string mant = epos == std::string::npos ? shortest : shortest.substr(0, epos);
    size_t dot = mant.find('.');
    std::string intPart = dot == std::string::npos ? mant : mant.substr(0, dot);
    std::string fracPart = dot == std::string::npos ? "" : mant.substr(dot + 1);
    digits = intPart + fracPart;
    point = int(intPart.size());
    if (epos != std::string::npos) point += std::stoi(shortest.substr(epos + 1));
    size_t nz = digits.find_first_not_of('0');
    point -= int(nz);
    digits = digits.substr(nz);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    m = digits;
    e = point - 1;
    fd = int(digits.size()) - 1;
  }
  if (fd != 0) m = m.substr(0, 1) + "." + m.substr(1);
  return Value(ascii(s + m + exponentSuffix(e)));
}

// 21.1.3.5 Number.prototype.toPrecision ( precision )
Result<Value> NumberPrototype_toPrecision(NativeCall& call) {
  double x = TRY(thisNumberValue(call.thisValue));
  if (call.args[0].isUndefined()) return Value(NumberToString(x));
  double pd = TRY(ToIntegerOrInfinity(call.args[0]));
  if (!std::isfinite(x)) return Value(NumberToString(x));
  if (pd < 1 || pd > 100) return ThrowRangeError("toPrecision() argument must be between 1 and 100");
  int p = int(pd);
  std::string s;
  if (x < 0) {
    s = "-";
    x = -x;
  }
  std::string m;
  int e = 0;
  if (x == 0) {
    m = std::string(size_t(p), '0');
  } else {
    std::string digits;
    int point;
    exactDecimal(x, digits, point);
    m = roundSignificant(digits, point, p, e);
    if (e < -6 || e >= p) {
      if (p != 1) m = m.substr(0, 1) + "." + m.substr(1);
      return Value(ascii(s + m + exponentSuffix(e)));
    }
  }
  if (e == p - 1) return Value(ascii(s + m));
  if (e >= 0) m = m.substr(0, size_t(e + 1)) + "." + m.substr(size_t(e + 1));
  else m = "0." + std::string(size_t(-(e + 1)), '0') + m;
  return Value(ascii(s + m));
}

// 21.1.3.6 Number.prototype.toString ( [ radix ] )
Result<Value> NumberPrototype_toString(NativeCall& call) {
  double x = TRY(thisNumberValue(call.thisValue));
  double radix = 10;
  if (!call.args[0].isUndefined()) radix = TRY(ToIntegerOrInfinity(call.args[0]));
  if (radix < 2 || radix > 36) return ThrowRangeError("toString() radix must be between 2 and 36");
  return Value(NumberToString(x, int(radix)));
}

// ECMA-402 20.2.1 Number.prototype.toLocaleString ( [ locales [ , options ] ] )
Result<Value> NumberPrototype_toLocaleString(NativeCall& call) {
  double x = TRY(thisNumberValue(call.thisValue));
  return intl::numberToLocaleString(num(x), call.args[0], call.args[1]);
}

Result<Value> NumberPrototype_valueOf(NativeCall& call) { return num(TRY(thisNumberValue(call.thisValue))); }

void installNumber(Realm& realm) {
  Object* proto = make<NumberObject>(realm.intrinsic(Intrinsic::ObjectPrototype), 0.0);
  BuiltinFunction* ctor =
      defineConstructor(realm, Intrinsic::Number, Number_construct, 1, PropertyKey(u"Number"), proto, Intrinsic::NumberPrototype);
  defineValue(ctor, PropertyKey(u"EPSILON"), num(2.220446049250313e-16), Attr::None);
  defineValue(ctor, PropertyKey(u"MAX_SAFE_INTEGER"), num(9007199254740991.0), Attr::None);
  defineValue(ctor, PropertyKey(u"MAX_VALUE"), num(1.7976931348623157e308), Attr::None);
  defineValue(ctor, PropertyKey(u"MIN_SAFE_INTEGER"), num(-9007199254740991.0), Attr::None);
  defineValue(ctor, PropertyKey(u"MIN_VALUE"), num(5e-324), Attr::None);
  defineValue(ctor, PropertyKey(u"NaN"), num(NAN), Attr::None);
  defineValue(ctor, PropertyKey(u"NEGATIVE_INFINITY"), num(-INFINITY), Attr::None);
  defineValue(ctor, PropertyKey(u"POSITIVE_INFINITY"), num(INFINITY), Attr::None);
  defineFunction(realm, ctor, PropertyKey(u"isFinite"), 1, Number_isFinite);
  defineFunction(realm, ctor, PropertyKey(u"isInteger"), 1, Number_isInteger);
  defineFunction(realm, ctor, PropertyKey(u"isNaN"), 1, Number_isNaN);
  defineFunction(realm, ctor, PropertyKey(u"isSafeInteger"), 1, Number_isSafeInteger);
  defineValue(ctor, PropertyKey(u"parseFloat"), realm.intrinsic(Intrinsic::parseFloat));
  defineValue(ctor, PropertyKey(u"parseInt"), realm.intrinsic(Intrinsic::parseInt));

  defineFunction(realm, proto, PropertyKey(u"toExponential"), 1, NumberPrototype_toExponential);
  defineFunction(realm, proto, PropertyKey(u"toFixed"), 1, NumberPrototype_toFixed);
  defineFunction(realm, proto, PropertyKey(u"toLocaleString"), 0, NumberPrototype_toLocaleString);
  defineFunction(realm, proto, PropertyKey(u"toPrecision"), 1, NumberPrototype_toPrecision);
  defineFunction(realm, proto, PropertyKey(u"toString"), 1, NumberPrototype_toString);
  defineFunction(realm, proto, PropertyKey(u"valueOf"), 0, NumberPrototype_valueOf);
}

}  // namespace

SNOWMAN_BUILTIN(Number, kOrderDefault, installNumber);

}  // namespace snowman::builtins
