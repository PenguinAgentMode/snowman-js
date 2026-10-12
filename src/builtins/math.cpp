// The Math object (21.3).
#include <bit>
#include <chrono>
#include <cmath>

#include "builtins/util.h"
#include "runtime/bigint.h"

namespace snowman::builtins {

namespace {

using UnaryFn = double (*)(double);

template <UnaryFn F>
Result<Value> unary(NativeCall& call) {
  double x = TRY(ToNumber(call.args[0]));
  return num(F(x));
}

double jsRound(double x) {
  if (!std::isfinite(x) || x == 0) return x;
  if (x > 0 && x < 0.5) return 0.0;
  if (x < 0 && x >= -0.5) return -0.0;
  if (std::fabs(x) >= 4503599627370496.0) return x;
  double r = std::floor(x);
  if (x - r >= 0.5) r += 1;
  return r;
}

double jsSign(double x) {
  if (std::isnan(x) || x == 0) return x;
  return x > 0 ? 1 : -1;
}

double jsFround(double x) { return double(float(x)); }

// Round to IEEE 754 binary16 (round half to even), returned as a double.
double f16round(double x) {
  if (std::isnan(x) || x == 0 || std::isinf(x)) return x;
  double a = std::fabs(x);
  double result;
  if (a >= 65520.0) {
    result = INFINITY;
  } else if (a < std::ldexp(1.0, -14)) {
    result = std::nearbyint(std::ldexp(a, 24)) * std::ldexp(1.0, -24);
  } else {
    int e;
    std::frexp(a, &e);  // a = m × 2^e, m in [0.5, 1)
    int shift = e - 1 - 10;
    result = std::ldexp(std::nearbyint(std::ldexp(a, -shift)), shift);
  }
  return std::signbit(x) ? -result : result;
}

double jsTrunc(double x) { return std::trunc(x); }
double jsCbrt(double x) { return std::cbrt(x); }
double jsAtanh(double x) { return std::atanh(x); }

Result<Value> Math_atan2(NativeCall& call) {
  double y = TRY(ToNumber(call.args[0]));
  double x = TRY(ToNumber(call.args[1]));
  return num(std::atan2(y, x));
}

Result<Value> Math_pow(NativeCall& call) {
  double base = TRY(ToNumber(call.args[0]));
  double exponent = TRY(ToNumber(call.args[1]));
  return num(NumberExponentiate(base, exponent));
}

Result<Value> Math_clz32(NativeCall& call) {
  uint32_t n = TRY(ToUint32(call.args[0]));
  return num(std::countl_zero(n));
}

Result<Value> Math_imul(NativeCall& call) {
  uint32_t a = TRY(ToUint32(call.args[0]));
  uint32_t b = TRY(ToUint32(call.args[1]));
  return num(double(int32_t(a * b)));
}

// 21.3.2.24 Math.max ( ...args ), 21.3.2.25 Math.min ( ...args )
Result<Value> minMax(NativeCall& call, bool isMax) {
  std::vector<double> coerced;
  for (Value arg : call.args) coerced.push_back(TRY(ToNumber(arg)));
  double result = isMax ? -INFINITY : INFINITY;
  for (double n : coerced) {
    if (std::isnan(n)) return num(NAN);
    if (n == 0 && result == 0) {
      if (isMax ? !std::signbit(n) : std::signbit(n)) result = n;
    } else if (isMax ? n > result : n < result) {
      result = n;
    }
  }
  return num(result);
}

Result<Value> Math_max(NativeCall& call) { return minMax(call, true); }
Result<Value> Math_min(NativeCall& call) { return minMax(call, false); }

// 21.3.2.18 Math.hypot ( ...args )
Result<Value> Math_hypot(NativeCall& call) {
  std::vector<double> coerced;
  for (Value arg : call.args) coerced.push_back(TRY(ToNumber(arg)));
  for (double n : coerced) {
    if (std::isinf(n)) return num(INFINITY);
  }
  bool onlyZero = true;
  double largest = 0;
  for (double n : coerced) {
    if (std::isnan(n)) return num(NAN);
    if (n != 0) onlyZero = false;
    largest = std::max(largest, std::fabs(n));
  }
  if (onlyZero) return num(0);
  double sum = 0, compensation = 0;
  for (double n : coerced) {
    double scaled = n / largest;
    double term = scaled * scaled - compensation;
    double t = sum + term;
    compensation = (t - sum) - term;
    sum = t;
  }
  return num(std::sqrt(sum) * largest);
}

// The exact sum of finite doubles, as an integer count of 2^-1074 (the smallest subnormal).
BigInt* scaledExact(double n) {
  uint64_t bits = std::bit_cast<uint64_t>(n);
  int exp = int((bits >> 52) & 0x7FF);
  uint64_t mant = bits & ((uint64_t(1) << 52) - 1);
  if (exp == 0) exp = 1;
  else mant |= uint64_t(1) << 52;
  BigInt* m = BigInt::leftShift(BigInt::fromUint64(mant), BigInt::fromInt64(exp - 1));
  return std::signbit(n) ? BigInt::unaryMinus(m) : m;
}

// The double nearest to sum × 2^-1074 (ties to even).
double unscale(const BigInt* sum) {
  const std::vector<uint32_t>& mag = sum->magnitude();
  if (mag.empty()) return 0;
  uint64_t bitLength = uint64_t(mag.size() - 1) * 32 + uint64_t(32 - std::countl_zero(mag.back()));
  double result;
  if (bitLength <= 64) {
    uint64_t v = mag[0] | (mag.size() > 1 ? uint64_t(mag[1]) << 32 : 0);
    result = std::ldexp(double(v), -1074);
  } else {
    // The top 64 bits with a sticky bit for everything below: one correctly rounded conversion.
    uint64_t shift = bitLength - 64;
    BigInt* top = BigInt::signedRightShift(sum->isNegative() ? BigInt::unaryMinus(sum) : sum, BigInt::fromUint64(shift));
    uint64_t v = top->toUint64();
    bool sticky = false;
    for (uint64_t i = 0; i < shift && !sticky; i++) sticky = (mag[i / 32] >> (i % 32)) & 1;
    if (sticky) v |= 1;
    result = std::ldexp(double(v), int(shift) - 1074);
  }
  return sum->isNegative() ? -result : result;
}

// Math.sumPrecise ( items )
Result<Value> Math_sumPrecise(NativeCall& call) {
  TRY(RequireObjectCoercible(call.args[0]));
  IteratorRecord record = TRY(GetIterator(call.args[0], IteratorKind::Sync));
  enum class State { MinusZero, Finite, PlusInfinity, MinusInfinity, NotANumber } state = State::MinusZero;
  BigInt* sum = BigInt::fromInt64(0);
  double count = 0;
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(record));
    if (!next) break;
    count++;
    if (count >= 9007199254740992.0) return closeIteratorWithThrow(record, ThrowRangeError("Math.sumPrecise: too many values").value);
    if (!next->isNumber()) return closeIteratorWithThrow(record, ThrowTypeError("Math.sumPrecise: value is not a number").value);
    double n = next->asNumber();
    if (state == State::NotANumber) continue;
    if (std::isnan(n)) state = State::NotANumber;
    else if (n == INFINITY) state = state == State::MinusInfinity ? State::NotANumber : State::PlusInfinity;
    else if (n == -INFINITY) state = state == State::PlusInfinity ? State::NotANumber : State::MinusInfinity;
    else if (!(n == 0 && std::signbit(n)) && (state == State::MinusZero || state == State::Finite)) {
      state = State::Finite;
      sum = BigInt::add(sum, scaledExact(n));
    }
  }
  switch (state) {
    case State::NotANumber: return num(NAN);
    case State::PlusInfinity: return num(INFINITY);
    case State::MinusInfinity: return num(-INFINITY);
    case State::MinusZero: return num(-0.0);
    case State::Finite: return num(unscale(sum));
  }
  return num(NAN);
}

uint64_t randomState[2];

// 21.3.2.27 Math.random ( ): xorshift128+.
Result<Value> Math_random(NativeCall&) {
  uint64_t s1 = randomState[0];
  const uint64_t s0 = randomState[1];
  randomState[0] = s0;
  s1 ^= s1 << 23;
  randomState[1] = s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26);
  uint64_t bits = (randomState[1] + s0) >> 11;
  return num(double(bits) / 9007199254740992.0);
}

void installMath(Realm& realm) {
  Object* math = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  realm.setIntrinsic(Intrinsic::Math, math);
  defineGlobal(realm, PropertyKey(u"Math"), math);
  defineValue(math, PropertyKey(u"E"), num(2.718281828459045), Attr::None);
  defineValue(math, PropertyKey(u"LN10"), num(2.302585092994046), Attr::None);
  defineValue(math, PropertyKey(u"LN2"), num(0.6931471805599453), Attr::None);
  defineValue(math, PropertyKey(u"LOG10E"), num(0.4342944819032518), Attr::None);
  defineValue(math, PropertyKey(u"LOG2E"), num(1.4426950408889634), Attr::None);
  defineValue(math, PropertyKey(u"PI"), num(3.141592653589793), Attr::None);
  defineValue(math, PropertyKey(u"SQRT1_2"), num(0.7071067811865476), Attr::None);
  defineValue(math, PropertyKey(u"SQRT2"), num(1.4142135623730951), Attr::None);
  defineValue(math, PropertyKey(vm().symbols.toStringTag), str(u"Math"), Attr::Configurable);

  struct Fn {
    const char16_t* name;
    int length;
    NativeFn fn;
  };
  const Fn fns[] = {
      {u"abs", 1, unary<std::fabs>},
      {u"acos", 1, unary<std::acos>},
      {u"acosh", 1, unary<std::acosh>},
      {u"asin", 1, unary<std::asin>},
      {u"asinh", 1, unary<std::asinh>},
      {u"atan", 1, unary<std::atan>},
      {u"atanh", 1, unary<jsAtanh>},
      {u"atan2", 2, Math_atan2},
      {u"cbrt", 1, unary<jsCbrt>},
      {u"ceil", 1, unary<std::ceil>},
      {u"clz32", 1, Math_clz32},
      {u"cos", 1, unary<std::cos>},
      {u"cosh", 1, unary<std::cosh>},
      {u"exp", 1, unary<std::exp>},
      {u"expm1", 1, unary<std::expm1>},
      {u"f16round", 1, unary<f16round>},
      {u"floor", 1, unary<std::floor>},
      {u"fround", 1, unary<jsFround>},
      {u"hypot", 2, Math_hypot},
      {u"imul", 2, Math_imul},
      {u"log", 1, unary<std::log>},
      {u"log1p", 1, unary<std::log1p>},
      {u"log10", 1, unary<std::log10>},
      {u"log2", 1, unary<std::log2>},
      {u"max", 2, Math_max},
      {u"min", 2, Math_min},
      {u"pow", 2, Math_pow},
      {u"random", 0, Math_random},
      {u"round", 1, unary<jsRound>},
      {u"sign", 1, unary<jsSign>},
      {u"sin", 1, unary<std::sin>},
      {u"sinh", 1, unary<std::sinh>},
      {u"sqrt", 1, unary<std::sqrt>},
      {u"sumPrecise", 1, Math_sumPrecise},
      {u"tan", 1, unary<std::tan>},
      {u"tanh", 1, unary<std::tanh>},
      {u"trunc", 1, unary<jsTrunc>},
  };
  for (const Fn& f : fns) defineFunction(realm, math, PropertyKey(f.name), f.length, f.fn);

  if (!randomState[0] && !randomState[1]) {
    uint64_t seed = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    randomState[0] = seed * 0x9E3779B97F4A7C15ull | 1;
    randomState[1] = (seed ^ 0xDEADBEEFCAFEBABEull) * 0xBF58476D1CE4E5B9ull | 2;
  }
}

}  // namespace

SNOWMAN_BUILTIN(Math, kOrderDefault, installMath);

}  // namespace snowman::builtins
