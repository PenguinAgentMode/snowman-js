// 7.1 Type conversion.
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "runtime/bigint.h"
#include "runtime/operations.h"
#include "runtime/symbol.h"
#include "runtime/vm.h"
#include "runtime/wrappers.h"

namespace snowman {

// 7.1.1 ToPrimitive
Result<Value> ToPrimitive(Value input, PreferredType preferredType) {
  if (!input.isObject()) return input;
  Value exoticToPrim = TRY(GetMethod(input, PropertyKey(vm().symbols.toPrimitive)));
  if (!exoticToPrim.isUndefined()) {
    const char16_t* hint = preferredType == PreferredType::None     ? u"default"
                           : preferredType == PreferredType::String ? u"string"
                                                                    : u"number";
    Value result = TRY(Call(exoticToPrim, input, {Value(atom(hint))}));
    if (!result.isObject()) return result;
    return ThrowTypeError("Cannot convert object to primitive value");
  }
  if (preferredType == PreferredType::None) preferredType = PreferredType::Number;
  return OrdinaryToPrimitive(input.asObject(), preferredType);
}

// 7.1.1.1 OrdinaryToPrimitive
Result<Value> OrdinaryToPrimitive(Object* object, PreferredType hint) {
  const char16_t* names[2] = {u"valueOf", u"toString"};
  if (hint == PreferredType::String) std::swap(names[0], names[1]);
  for (const char16_t* name : names) {
    Value method = TRY(Get(object, PropertyKey(name)));
    if (IsCallable(method)) {
      Value result = TRY(Call(method, object));
      if (!result.isObject()) return result;
    }
  }
  return ThrowTypeError("Cannot convert object to primitive value");
}

// 7.1.2 ToBoolean
bool ToBoolean(Value argument) {
  switch (argument.type()) {
    case ValueType::Undefined:
    case ValueType::Null:
    case ValueType::Empty:
      return false;
    case ValueType::Boolean:
      return argument.asBoolean();
    case ValueType::Number: {
      double d = argument.asNumber();
      return !(d == 0 || std::isnan(d));
    }
    case ValueType::String:
      return argument.asString()->length() != 0;
    case ValueType::Symbol:
      return true;
    case ValueType::BigInt:
      return !argument.asBigInt()->isZero();
    case ValueType::Object:
      return !argument.asObject()->isHTMLDDA();  // B.3.6.1
  }
  return false;
}

// 7.1.3 ToNumeric
Result<Value> ToNumeric(Value value) {
  Value primValue = TRY(ToPrimitive(value, PreferredType::Number));
  if (primValue.isBigInt()) return primValue;
  return Value::number(TRY(ToNumber(primValue)));
}

// 7.1.4 ToNumber
Result<double> ToNumber(Value argument) {
  switch (argument.type()) {
    case ValueType::Undefined:
    case ValueType::Empty:
      return NAN;
    case ValueType::Null:
      return 0.0;
    case ValueType::Boolean:
      return argument.asBoolean() ? 1.0 : 0.0;
    case ValueType::Number:
      return argument.asNumber();
    case ValueType::String:
      return StringToNumber(argument.asString()->view());
    case ValueType::Symbol:
      return ThrowTypeError("Cannot convert a Symbol value to a number");
    case ValueType::BigInt:
      return ThrowTypeError("Cannot convert a BigInt value to a number");
    case ValueType::Object: {
      Value primValue = TRY(ToPrimitive(argument, PreferredType::Number));
      return ToNumber(primValue);
    }
  }
  return NAN;
}

namespace {

int digitOf(char16_t c) {
  if (c >= u'0' && c <= u'9') return c - u'0';
  if (c >= u'a' && c <= u'z') return c - u'a' + 10;
  if (c >= u'A' && c <= u'Z') return c - u'A' + 10;
  return 99;
}

double parseRadixInteger(std::u16string_view digits, int radix) {
  if (digits.empty()) return NAN;
  uint64_t v = 0;
  bool exact = true;
  for (char16_t c : digits) {
    int d = digitOf(c);
    if (d >= radix) return NAN;
    if (v > (uint64_t(1) << 52)) {
      exact = false;
      break;
    }
    v = v * radix + d;
  }
  if (exact) return double(v);
  std::string ascii(digits.begin(), digits.end());
  return BigInt::fromDigits(ascii, radix)->toDouble();
}

}  // namespace

// 7.1.4.1.1 StringToNumber
double StringToNumber(std::u16string_view text) {
  size_t b = 0, e = text.size();
  while (b < e && isWhiteSpaceOrLineTerminator(text[b])) b++;
  while (e > b && isWhiteSpaceOrLineTerminator(text[e - 1])) e--;
  std::u16string_view s = text.substr(b, e - b);
  if (s.empty()) return 0;
  if (s.size() > 2 && s[0] == u'0') {
    char16_t p = s[1];
    if (p == u'x' || p == u'X') return parseRadixInteger(s.substr(2), 16);
    if (p == u'o' || p == u'O') return parseRadixInteger(s.substr(2), 8);
    if (p == u'b' || p == u'B') return parseRadixInteger(s.substr(2), 2);
  }
  size_t i = 0;
  bool negative = false;
  if (s[0] == u'+' || s[0] == u'-') {
    negative = s[0] == u'-';
    i = 1;
  }
  std::u16string_view rest = s.substr(i);
  if (rest == u"Infinity") return negative ? -INFINITY : INFINITY;
  // StrUnsignedDecimalLiteral: digits [. digits] [exp] | . digits [exp]
  size_t j = 0, n = rest.size();
  size_t intDigits = 0, fracDigits = 0;
  while (j < n && rest[j] >= u'0' && rest[j] <= u'9') j++, intDigits++;
  if (j < n && rest[j] == u'.') {
    j++;
    while (j < n && rest[j] >= u'0' && rest[j] <= u'9') j++, fracDigits++;
  }
  if (intDigits + fracDigits == 0) return NAN;
  if (j < n && (rest[j] == u'e' || rest[j] == u'E')) {
    j++;
    if (j < n && (rest[j] == u'+' || rest[j] == u'-')) j++;
    size_t expDigits = 0;
    while (j < n && rest[j] >= u'0' && rest[j] <= u'9') j++, expDigits++;
    if (expDigits == 0) return NAN;
  }
  if (j != n) return NAN;
  std::string ascii(rest.begin(), rest.end());
  double value = std::strtod(ascii.c_str(), nullptr);
  return negative ? -value : value;
}

// 7.1.5 ToIntegerOrInfinity
Result<double> ToIntegerOrInfinity(Value argument) {
  double number = TRY(ToNumber(argument));
  if (std::isnan(number) || number == 0) return 0.0;
  if (std::isinf(number)) return number;
  return std::trunc(number) + 0.0;
}

double NumberExponentiate(double base, double exponent) {
  if (std::isnan(exponent)) return NAN;
  if (exponent == 0) return 1;
  if ((base == 1 || base == -1) && std::isinf(exponent)) return NAN;
  return std::pow(base, exponent);
}

int32_t DoubleToInt32(double d) {
  if (d >= -2147483648.0 && d <= 2147483647.0) {
    int32_t i = int32_t(d);
    if (double(i) == d) return i;
  }
  if (!std::isfinite(d) || d == 0) return 0;
  double m = std::fmod(std::trunc(d), 4294967296.0);
  if (m < 0) m += 4294967296.0;
  uint32_t u = uint32_t(m);
  return int32_t(u);
}

uint32_t DoubleToUint32(double d) { return uint32_t(DoubleToInt32(d)); }

// 7.1.6–7.1.11
Result<int32_t> ToInt32(Value argument) { return DoubleToInt32(TRY(ToNumber(argument))); }
Result<uint32_t> ToUint32(Value argument) { return DoubleToUint32(TRY(ToNumber(argument))); }
Result<int16_t> ToInt16(Value argument) { return int16_t(DoubleToUint32(TRY(ToNumber(argument)))); }
Result<uint16_t> ToUint16(Value argument) { return uint16_t(DoubleToUint32(TRY(ToNumber(argument)))); }
Result<int8_t> ToInt8(Value argument) { return int8_t(DoubleToUint32(TRY(ToNumber(argument)))); }
Result<uint8_t> ToUint8(Value argument) { return uint8_t(DoubleToUint32(TRY(ToNumber(argument)))); }

// 7.1.12 ToUint8Clamp
Result<uint8_t> ToUint8Clamp(Value argument) {
  double number = TRY(ToNumber(argument));
  if (std::isnan(number) || number <= 0) return uint8_t(0);
  if (number >= 255) return uint8_t(255);
  double f = std::floor(number);
  if (f + 0.5 < number) return uint8_t(f + 1);
  if (number < f + 0.5) return uint8_t(f);
  return uint8_t(int(f) % 2 ? f + 1 : f);
}

// 7.1.13 ToBigInt
Result<BigInt*> ToBigInt(Value argument) {
  Value prim = TRY(ToPrimitive(argument, PreferredType::Number));
  switch (prim.type()) {
    case ValueType::Undefined:
    case ValueType::Null:
    case ValueType::Empty:
      return ThrowTypeError("Cannot convert to BigInt");
    case ValueType::Boolean:
      return BigInt::fromInt64(prim.asBoolean() ? 1 : 0);
    case ValueType::BigInt:
      return prim.asBigInt();
    case ValueType::Number:
      return ThrowTypeError("Cannot convert a Number to a BigInt");
    case ValueType::String: {
      auto n = BigInt::fromString(prim.asString()->view());
      if (!n) return ThrowSyntaxError("Cannot convert string to BigInt");
      return *n;
    }
    case ValueType::Symbol:
      return ThrowTypeError("Cannot convert a Symbol to a BigInt");
    case ValueType::Object:
      break;
  }
  return ThrowTypeError("Cannot convert to BigInt");
}

// 7.1.15 ToBigInt64, 7.1.16 ToBigUint64
Result<int64_t> ToBigInt64(Value argument) { return TRY(ToBigInt(argument))->toInt64(); }
Result<uint64_t> ToBigUint64(Value argument) { return TRY(ToBigInt(argument))->toUint64(); }

// 7.1.17 ToString
Result<String*> ToString(Value argument) {
  switch (argument.type()) {
    case ValueType::Undefined:
    case ValueType::Empty:
      return atom(u"undefined");
    case ValueType::Null:
      return atom(u"null");
    case ValueType::Boolean:
      return atom(argument.asBoolean() ? u"true" : u"false");
    case ValueType::Number:
      return NumberToString(argument.asNumber());
    case ValueType::String:
      return argument.asString();
    case ValueType::Symbol:
      return ThrowTypeError("Cannot convert a Symbol value to a string");
    case ValueType::BigInt:
      return newString(argument.asBigInt()->toString(10));
    case ValueType::Object: {
      Value primValue = TRY(ToPrimitive(argument, PreferredType::String));
      return ToString(primValue);
    }
  }
  return emptyString();
}

// 7.1.18 ToObject
Result<Object*> ToObject(Value argument) {
  Realm* realm = vm().currentRealm();
  switch (argument.type()) {
    case ValueType::Undefined:
    case ValueType::Null:
    case ValueType::Empty:
      return ThrowTypeError("Cannot convert undefined or null to object");
    case ValueType::Boolean:
      return make<BooleanObject>(realm->intrinsic(Intrinsic::BooleanPrototype), argument.asBoolean());
    case ValueType::Number:
      return make<NumberObject>(realm->intrinsic(Intrinsic::NumberPrototype), argument.asNumber());
    case ValueType::String:
      return make<StringObject>(realm->intrinsic(Intrinsic::StringPrototype), argument.asString());
    case ValueType::Symbol:
      return make<SymbolObject>(realm->intrinsic(Intrinsic::SymbolPrototype), argument.asSymbol());
    case ValueType::BigInt:
      return make<BigIntObject>(realm->intrinsic(Intrinsic::BigIntPrototype), argument.asBigInt());
    case ValueType::Object:
      return argument.asObject();
  }
  return ThrowTypeError("Cannot convert to object");
}

// 7.1.19 ToPropertyKey
Result<PropertyKey> ToPropertyKey(Value argument) {
  if (argument.isString()) return PropertyKey(argument.asString());
  if (argument.isSymbol()) return PropertyKey(argument.asSymbol());
  if (argument.isNumber()) {
    double d = argument.asNumber();
    if (d >= 0 && d <= PropertyKey::kMaxIndex && d == std::floor(d) && !(d == 0 && std::signbit(d)))
      return PropertyKey::index(uint32_t(d));
  }
  Value key = TRY(ToPrimitive(argument, PreferredType::String));
  if (key.isSymbol()) return PropertyKey(key.asSymbol());
  return PropertyKey(TRY(ToString(key)));
}

// 7.1.20 ToLength
Result<double> ToLength(Value argument) {
  double len = TRY(ToIntegerOrInfinity(argument));
  if (len <= 0) return 0.0;
  return std::min(len, 9007199254740991.0);
}

// 7.1.22 ToIndex
Result<double> ToIndex(Value value) {
  double integer = TRY(ToIntegerOrInfinity(value));
  if (integer < 0 || integer > 9007199254740991.0) return ThrowRangeError("Invalid index");
  return integer;
}

// 7.1.21 CanonicalNumericIndexString
std::optional<double> CanonicalNumericIndexString(String* argument) {
  if (argument->view() == u"-0") return -0.0;
  double n = StringToNumber(argument->view());
  String* s = NumberToString(n);
  if (s->view() == argument->view()) return n;
  return std::nullopt;
}

namespace {

// The shortest round-trip decimal digits of a finite positive double and its exponent n (value =
// 0.d1d2...dk × 10^n), as Number::toString defines them.
void shortestDigits(double x, std::string& digits, int& n) {
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::scientific);
  std::string s(buf, r.ptr);
  size_t e = s.find('e');
  std::string mant = s.substr(0, e);
  int exp = std::atoi(s.c_str() + e + 1);
  digits.clear();
  for (char c : mant) {
    if (c != '.') digits.push_back(c);
  }
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  n = exp + 1;
}

std::string radixString(double value, int radix) {
  static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  constexpr int kBufferSize = 2200;
  char buffer[kBufferSize];
  int integerCursor = kBufferSize / 2;
  int fractionCursor = integerCursor;
  bool negative = value < 0;
  if (negative) value = -value;
  double integer = std::floor(value);
  double fraction = value - integer;
  double delta = 0.5 * (std::nextafter(value, INFINITY) - value);
  delta = std::max(std::nextafter(0.0, 1.0), delta);
  if (fraction >= delta) {
    buffer[fractionCursor++] = '.';
    do {
      fraction *= radix;
      delta *= radix;
      int digit = int(fraction);
      buffer[fractionCursor++] = chars[digit];
      fraction -= digit;
      if (fraction > 0.5 || (fraction == 0.5 && (digit & 1))) {
        if (fraction + delta > 1) {
          while (true) {
            fractionCursor--;
            if (fractionCursor == kBufferSize / 2) {
              integer += 1;
              break;
            }
            char c = buffer[fractionCursor];
            int d = c > '9' ? (c - 'a' + 10) : (c - '0');
            if (d + 1 < radix) {
              buffer[fractionCursor++] = chars[d + 1];
              break;
            }
          }
          break;
        }
      }
    } while (fraction >= delta);
  }
  // Integer digits; digits below the double's precision are zero.
  auto exponentOf = [](double d) {
    uint64_t bits = std::bit_cast<uint64_t>(d);
    return int((bits >> 52) & 0x7FF) - 0x3FF - 52;
  };
  while (exponentOf(integer / radix) > 0) {
    integer /= radix;
    buffer[--integerCursor] = '0';
  }
  do {
    double remainder = std::fmod(integer, radix);
    buffer[--integerCursor] = chars[int(remainder)];
    integer = (integer - remainder) / radix;
  } while (integer > 0);
  if (negative) buffer[--integerCursor] = '-';
  return std::string(buffer + integerCursor, buffer + fractionCursor);
}

}  // namespace

// 6.1.6.1.20 Number::toString
String* NumberToString(double x, int radix) {
  if (std::isnan(x)) return atom(u"NaN");
  if (x == 0) return atom(u"0");
  if (std::isinf(x)) return atom(x < 0 ? u"-Infinity" : u"Infinity");
  if (radix == 10 && x >= 0 && x < 1024 && x == std::floor(x)) return PropertyKey::index(uint32_t(x)).toValue().asString();
  std::string out;
  if (radix != 10) {
    out = radixString(x, radix);
  } else {
    if (x < 0) {
      out.push_back('-');
      x = -x;
    }
    std::string digits;
    int n;
    shortestDigits(x, digits, n);
    int k = int(digits.size());
    if (k <= n && n <= 21) {
      out += digits;
      out.append(size_t(n - k), '0');
    } else if (0 < n && n <= 21) {
      out += digits.substr(0, size_t(n));
      out.push_back('.');
      out += digits.substr(size_t(n));
    } else if (-6 < n && n <= 0) {
      out += "0.";
      out.append(size_t(-n), '0');
      out += digits;
    } else {
      int e = n - 1;
      out.push_back(digits[0]);
      if (k > 1) {
        out.push_back('.');
        out += digits.substr(1);
      }
      out.push_back('e');
      out.push_back(e < 0 ? '-' : '+');
      out += std::to_string(e < 0 ? -e : e);
    }
  }
  return newString(std::u16string(out.begin(), out.end()));
}

}  // namespace snowman
