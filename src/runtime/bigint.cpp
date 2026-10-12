// Arbitrary-precision integers (6.1.6.2): sign + little-endian base-2^32 magnitude.
#include "runtime/bigint.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "runtime/string.h"

namespace snowman {

namespace {

using Mag = std::vector<uint32_t>;

void trim(Mag& m) {
  while (!m.empty() && m.back() == 0) m.pop_back();
}

int cmpMag(const Mag& a, const Mag& b) {
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  for (size_t i = a.size(); i-- > 0;) {
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  }
  return 0;
}

Mag addMag(const Mag& a, const Mag& b) {
  const Mag& x = a.size() >= b.size() ? a : b;
  const Mag& y = a.size() >= b.size() ? b : a;
  Mag r(x.size() + 1);
  uint64_t carry = 0;
  for (size_t i = 0; i < x.size(); i++) {
    uint64_t s = uint64_t(x[i]) + (i < y.size() ? y[i] : 0) + carry;
    r[i] = uint32_t(s);
    carry = s >> 32;
  }
  r[x.size()] = uint32_t(carry);
  trim(r);
  return r;
}

// a >= b
Mag subMag(const Mag& a, const Mag& b) {
  Mag r(a.size());
  int64_t borrow = 0;
  for (size_t i = 0; i < a.size(); i++) {
    int64_t d = int64_t(a[i]) - (i < b.size() ? b[i] : 0) - borrow;
    if (d < 0) {
      d += int64_t(1) << 32;
      borrow = 1;
    } else {
      borrow = 0;
    }
    r[i] = uint32_t(d);
  }
  trim(r);
  return r;
}

Mag mulMag(const Mag& a, const Mag& b) {
  if (a.empty() || b.empty()) return {};
  Mag r(a.size() + b.size());
  for (size_t i = 0; i < a.size(); i++) {
    uint64_t carry = 0;
    uint64_t ai = a[i];
    for (size_t j = 0; j < b.size(); j++) {
      uint64_t t = ai * b[j] + r[i + j] + carry;
      r[i + j] = uint32_t(t);
      carry = t >> 32;
    }
    size_t k = i + b.size();
    while (carry) {
      uint64_t t = uint64_t(r[k]) + carry;
      r[k] = uint32_t(t);
      carry = t >> 32;
      k++;
    }
  }
  trim(r);
  return r;
}

// Divides m in place by a small divisor, returns the remainder.
uint32_t divSmall(Mag& m, uint32_t d) {
  uint64_t rem = 0;
  for (size_t i = m.size(); i-- > 0;) {
    uint64_t cur = (rem << 32) | m[i];
    m[i] = uint32_t(cur / d);
    rem = cur % d;
  }
  trim(m);
  return uint32_t(rem);
}

void mulAddSmall(Mag& m, uint32_t mul, uint32_t add) {
  uint64_t carry = add;
  for (auto& limb : m) {
    uint64_t t = uint64_t(limb) * mul + carry;
    limb = uint32_t(t);
    carry = t >> 32;
  }
  if (carry) m.push_back(uint32_t(carry));
}

Mag shlMag(const Mag& a, uint64_t n) {
  if (a.empty()) return {};
  size_t limbs = size_t(n / 32);
  unsigned bits = unsigned(n % 32);
  Mag r(a.size() + limbs + 1, 0);
  for (size_t i = 0; i < a.size(); i++) {
    uint64_t v = uint64_t(a[i]) << bits;
    r[i + limbs] |= uint32_t(v);
    r[i + limbs + 1] |= uint32_t(v >> 32);
  }
  trim(r);
  return r;
}

Mag shrMag(const Mag& a, uint64_t n) {
  size_t limbs = size_t(std::min<uint64_t>(n / 32, a.size()));
  unsigned bits = unsigned(n % 32);
  if (limbs >= a.size()) return {};
  Mag r(a.size() - limbs);
  for (size_t i = 0; i < r.size(); i++) {
    uint64_t lo = a[i + limbs];
    uint64_t hi = i + limbs + 1 < a.size() ? a[i + limbs + 1] : 0;
    r[i] = uint32_t(((hi << 32) | lo) >> bits);
  }
  trim(r);
  return r;
}

// Knuth algorithm D. b non-empty.
void divModMag(const Mag& a, const Mag& b, Mag& q, Mag& r) {
  if (cmpMag(a, b) < 0) {
    q.clear();
    r = a;
    return;
  }
  if (b.size() == 1) {
    q = a;
    uint32_t rem = divSmall(q, b[0]);
    r.clear();
    if (rem) r.push_back(rem);
    return;
  }
  unsigned s = std::countl_zero(b.back());
  Mag u = shlMag(a, s), v = shlMag(b, s);
  if (u.size() == a.size()) u.push_back(0);
  if (u.size() < a.size() + 1) u.resize(a.size() + 1, 0);
  size_t n = v.size(), m = u.size() - n;
  q.assign(m, 0);
  for (size_t j = m; j-- > 0;) {
    uint64_t num = (uint64_t(u[j + n]) << 32) | u[j + n - 1];
    uint64_t qhat = num / v[n - 1];
    uint64_t rhat = num % v[n - 1];
    while (qhat >= (uint64_t(1) << 32) || qhat * v[n - 2] > ((rhat << 32) | u[j + n - 2])) {
      qhat--;
      rhat += v[n - 1];
      if (rhat >= (uint64_t(1) << 32)) break;
    }
    int64_t borrow = 0;
    uint64_t carry = 0;
    for (size_t i = 0; i < n; i++) {
      uint64_t p = qhat * v[i] + carry;
      carry = p >> 32;
      int64_t t = int64_t(u[i + j]) - int64_t(uint32_t(p)) - borrow;
      borrow = t < 0 ? 1 : 0;
      u[i + j] = uint32_t(t);
    }
    int64_t t = int64_t(u[j + n]) - int64_t(carry) - borrow;
    u[j + n] = uint32_t(t);
    if (t < 0) {
      qhat--;
      uint64_t c = 0;
      for (size_t i = 0; i < n; i++) {
        uint64_t s2 = uint64_t(u[i + j]) + v[i] + c;
        u[i + j] = uint32_t(s2);
        c = s2 >> 32;
      }
      u[j + n] += uint32_t(c);
    }
    q[j] = uint32_t(qhat);
  }
  trim(q);
  u.resize(n);
  trim(u);
  r = shrMag(u, s);
}

uint64_t bitLength(const Mag& m) {
  if (m.empty()) return 0;
  return uint64_t(m.size() - 1) * 32 + (32 - std::countl_zero(m.back()));
}

// Two's complement of (negative, mag) in exactly `limbs` limbs.
Mag toTwos(bool negative, const Mag& mag, size_t limbs) {
  Mag r(limbs, 0);
  for (size_t i = 0; i < std::min(limbs, mag.size()); i++) r[i] = mag[i];
  if (negative) {
    uint64_t carry = 1;
    for (auto& limb : r) {
      uint64_t t = uint64_t(uint32_t(~limb)) + carry;
      limb = uint32_t(t);
      carry = t >> 32;
    }
  }
  return r;
}

BigInt* fromTwos(Mag r) {
  bool negative = !r.empty() && (r.back() & 0x80000000u);
  if (negative) {
    uint64_t carry = 1;
    for (auto& limb : r) {
      uint64_t t = uint64_t(uint32_t(~limb)) + carry;
      limb = uint32_t(t);
      carry = t >> 32;
    }
  }
  return make<BigInt>(negative, std::move(r));
}

template <class Op>
BigInt* bitwise(const BigInt* x, const BigInt* y, Op op) {
  size_t limbs = std::max(x->magnitude().size(), y->magnitude().size()) + 1;
  Mag a = toTwos(x->isNegative(), x->magnitude(), limbs);
  Mag b = toTwos(y->isNegative(), y->magnitude(), limbs);
  for (size_t i = 0; i < limbs; i++) a[i] = op(a[i], b[i]);
  return fromTwos(std::move(a));
}

int digitValue(char16_t c) {
  if (c >= u'0' && c <= u'9') return c - u'0';
  if (c >= u'a' && c <= u'z') return c - u'a' + 10;
  if (c >= u'A' && c <= u'Z') return c - u'A' + 10;
  return 99;
}

BigInt* signedResult(bool negative, Mag mag) { return make<BigInt>(negative, std::move(mag)); }

}  // namespace

BigInt::BigInt(bool negative, std::vector<uint32_t> magnitude) : negative_(negative), magnitude_(std::move(magnitude)) {
  trim(magnitude_);
  if (magnitude_.empty()) negative_ = false;
}

BigInt* BigInt::fromInt64(int64_t value) {
  uint64_t mag = value < 0 ? uint64_t(0) - uint64_t(value) : uint64_t(value);
  return make<BigInt>(value < 0, Mag{uint32_t(mag), uint32_t(mag >> 32)});
}

BigInt* BigInt::fromUint64(uint64_t value) { return make<BigInt>(false, Mag{uint32_t(value), uint32_t(value >> 32)}); }

BigInt* BigInt::fromDouble(double value) {
  bool negative = value < 0;
  double a = std::fabs(value);
  if (a < 1) return make<BigInt>(false, Mag{});
  uint64_t bits = std::bit_cast<uint64_t>(a);
  int exp = int((bits >> 52) & 0x7FF);
  uint64_t mant = (bits & ((uint64_t(1) << 52) - 1)) | (uint64_t(1) << 52);
  int e = exp - 1075;
  Mag m{uint32_t(mant), uint32_t(mant >> 32)};
  trim(m);
  if (e > 0) m = shlMag(m, uint64_t(e));
  else if (e < 0) m = shrMag(m, uint64_t(-e));
  return make<BigInt>(negative, std::move(m));
}

std::optional<BigInt*> BigInt::fromString(std::u16string_view text) {
  size_t b = 0, e = text.size();
  while (b < e && isWhiteSpaceOrLineTerminator(text[b])) b++;
  while (e > b && isWhiteSpaceOrLineTerminator(text[e - 1])) e--;
  std::u16string_view s = text.substr(b, e - b);
  if (s.empty()) return make<BigInt>(false, Mag{});
  int radix = 10;
  bool negative = false;
  if (s.size() > 2 && s[0] == u'0' && (s[1] == u'x' || s[1] == u'X' || s[1] == u'o' || s[1] == u'O' || s[1] == u'b' || s[1] == u'B')) {
    radix = (s[1] == u'x' || s[1] == u'X') ? 16 : (s[1] == u'o' || s[1] == u'O') ? 8 : 2;
    s = s.substr(2);
  } else if (s[0] == u'+' || s[0] == u'-') {
    negative = s[0] == u'-';
    s = s.substr(1);
  }
  if (s.empty()) return std::nullopt;
  Mag m;
  for (char16_t c : s) {
    int d = digitValue(c);
    if (d >= radix) return std::nullopt;
    mulAddSmall(m, uint32_t(radix), uint32_t(d));
  }
  trim(m);
  return make<BigInt>(negative, std::move(m));
}

BigInt* BigInt::fromDigits(std::string_view digits, int radix) {
  bool negative = false;
  if (!digits.empty() && digits[0] == '-') {
    negative = true;
    digits.remove_prefix(1);
  }
  Mag m;
  for (char c : digits) {
    if (c == '_') continue;
    int d = digitValue(char16_t(c));
    if (d >= radix) break;
    mulAddSmall(m, uint32_t(radix), uint32_t(d));
  }
  trim(m);
  return make<BigInt>(negative, std::move(m));
}

std::u16string BigInt::toString(int radix) const {
  if (magnitude_.empty()) return u"0";
  static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  uint32_t chunk = uint32_t(radix);
  int perChunk = 1;
  while (uint64_t(chunk) * radix <= 0xFFFFFFFFull) {
    chunk *= radix;
    perChunk++;
  }
  Mag m = magnitude_;
  std::string out;
  while (!m.empty()) {
    uint32_t rem = divSmall(m, chunk);
    for (int i = 0; i < perChunk; i++) {
      if (m.empty() && rem == 0) break;
      out.push_back(digits[rem % radix]);
      rem /= radix;
    }
  }
  if (negative_) out.push_back('-');
  std::reverse(out.begin(), out.end());
  return std::u16string(out.begin(), out.end());
}

double BigInt::toDouble() const {
  uint64_t len = bitLength(magnitude_);
  double result;
  if (len <= 64) {
    uint64_t v = magnitude_.empty() ? 0 : magnitude_[0];
    if (magnitude_.size() > 1) v |= uint64_t(magnitude_[1]) << 32;
    result = double(v);
  } else {
    Mag top = shrMag(magnitude_, len - 64);
    uint64_t v = uint64_t(top[0]) | (uint64_t(top.size() > 1 ? top[1] : 0) << 32);
    // Sticky bit: anything shifted out makes the value strictly above v, which matters only for ties.
    bool sticky = false;
    uint64_t dropped = len - 64;
    for (size_t i = 0; i < size_t(dropped / 32) && !sticky; i++) sticky = magnitude_[i] != 0;
    if (!sticky && dropped % 32) sticky = (magnitude_[dropped / 32] & ((1u << (dropped % 32)) - 1)) != 0;
    if (sticky) v |= 1;
    result = len - 64 > 2000 ? INFINITY : std::ldexp(double(v), int(len - 64));
  }
  return negative_ ? -result : result;
}

int64_t BigInt::toInt64() const { return int64_t(toUint64()); }

uint64_t BigInt::toUint64() const {
  uint64_t v = magnitude_.empty() ? 0 : magnitude_[0];
  if (magnitude_.size() > 1) v |= uint64_t(magnitude_[1]) << 32;
  return negative_ ? uint64_t(0) - v : v;
}

BigInt* BigInt::unaryMinus(const BigInt* x) { return signedResult(!x->negative_, x->magnitude_); }

BigInt* BigInt::bitwiseNot(const BigInt* x) {
  // ~x = -x - 1
  return subtract(unaryMinus(x), fromInt64(1));
}

BigInt* BigInt::exponentiate(const BigInt* base, const BigInt* exponent) {
  if (exponent->isZero()) return fromInt64(1);
  if (base->isZero()) return fromInt64(0);
  if (base->magnitude_.size() == 1 && base->magnitude_[0] == 1) {
    bool odd = exponent->magnitude_[0] & 1;
    return fromInt64(base->negative_ && odd ? -1 : 1);
  }
  Mag result{1};
  Mag b = base->magnitude_;
  uint64_t e = exponent->toUint64();
  bool odd = e & 1;
  while (e) {
    if (e & 1) result = mulMag(result, b);
    e >>= 1;
    if (e) b = mulMag(b, b);
  }
  return signedResult(base->negative_ && odd, std::move(result));
}

BigInt* BigInt::multiply(const BigInt* x, const BigInt* y) {
  return signedResult(x->negative_ != y->negative_, mulMag(x->magnitude_, y->magnitude_));
}

BigInt* BigInt::divide(const BigInt* x, const BigInt* y) {
  Mag q, r;
  divModMag(x->magnitude_, y->magnitude_, q, r);
  return signedResult(x->negative_ != y->negative_, std::move(q));
}

BigInt* BigInt::remainder(const BigInt* x, const BigInt* y) {
  Mag q, r;
  divModMag(x->magnitude_, y->magnitude_, q, r);
  return signedResult(x->negative_, std::move(r));
}

BigInt* BigInt::add(const BigInt* x, const BigInt* y) {
  if (x->negative_ == y->negative_) return signedResult(x->negative_, addMag(x->magnitude_, y->magnitude_));
  int c = cmpMag(x->magnitude_, y->magnitude_);
  if (c == 0) return fromInt64(0);
  if (c > 0) return signedResult(x->negative_, subMag(x->magnitude_, y->magnitude_));
  return signedResult(y->negative_, subMag(y->magnitude_, x->magnitude_));
}

BigInt* BigInt::subtract(const BigInt* x, const BigInt* y) { return add(x, unaryMinus(y)); }

BigInt* BigInt::leftShift(const BigInt* x, const BigInt* y) {
  if (y->negative_) return signedRightShift(x, unaryMinus(y));
  if (x->isZero()) return fromInt64(0);
  if (y->magnitude_.size() > 1) return signedResult(x->negative_, shlMag(x->magnitude_, uint64_t(1) << 40));
  return signedResult(x->negative_, shlMag(x->magnitude_, y->toUint64()));
}

BigInt* BigInt::signedRightShift(const BigInt* x, const BigInt* y) {
  if (y->negative_) return leftShift(x, unaryMinus(y));
  uint64_t n = y->magnitude_.size() > 2 ? ~uint64_t(0) : y->toUint64();
  if (!x->negative_) return signedResult(false, shrMag(x->magnitude_, n));
  // floor(x / 2^n) for x < 0: -(((|x| - 1) >> n) + 1)
  Mag m = subMag(x->magnitude_, Mag{1});
  m = shrMag(m, n);
  m = addMag(m, Mag{1});
  return signedResult(true, std::move(m));
}

BigInt* BigInt::bitwiseAnd(const BigInt* x, const BigInt* y) {
  return bitwise(x, y, [](uint32_t a, uint32_t b) { return a & b; });
}
BigInt* BigInt::bitwiseXor(const BigInt* x, const BigInt* y) {
  return bitwise(x, y, [](uint32_t a, uint32_t b) { return a ^ b; });
}
BigInt* BigInt::bitwiseOr(const BigInt* x, const BigInt* y) {
  return bitwise(x, y, [](uint32_t a, uint32_t b) { return a | b; });
}

BigInt* BigInt::asUintN(uint64_t bits, const BigInt* x) {
  if (bits == 0 || x->isZero()) return fromInt64(0);
  if (!x->negative_ && bitLength(x->magnitude_) <= bits) return const_cast<BigInt*>(x);
  size_t limbs = size_t((bits + 31) / 32);
  Mag r = toTwos(x->negative_, x->magnitude_, limbs);
  if (bits % 32) r.back() &= (1u << (bits % 32)) - 1;
  return make<BigInt>(false, std::move(r));
}

BigInt* BigInt::asIntN(uint64_t bits, const BigInt* x) {
  if (bits == 0 || x->isZero()) return fromInt64(0);
  if (bitLength(x->magnitude_) < bits) return const_cast<BigInt*>(x);
  BigInt* u = asUintN(bits, x);
  // u ≥ 2^(bits-1) → u - 2^bits
  if (bitLength(u->magnitude_) == bits) {
    Mag two = shlMag(Mag{1}, bits);
    return make<BigInt>(true, subMag(two, u->magnitude_));
  }
  return u;
}

int BigInt::compare(const BigInt* x, const BigInt* y) {
  if (x->negative_ != y->negative_) return x->negative_ ? -1 : 1;
  int c = cmpMag(x->magnitude_, y->magnitude_);
  return x->negative_ ? -c : c;
}

std::optional<int> BigInt::compareToNumber(const BigInt* x, double y) {
  if (std::isnan(y)) return std::nullopt;
  if (y == INFINITY) return -1;
  if (y == -INFINITY) return 1;
  double fy = std::floor(y);
  int c = compare(x, fromDouble(fy));
  if (c != 0) return c;
  return fy == y ? 0 : -1;
}

}  // namespace snowman
