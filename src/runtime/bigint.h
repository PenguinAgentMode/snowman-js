// BigInt: arbitrary-precision integers, sign + magnitude. INTERFACES.md section 4.
// The operations are the spec's BigInt::* (6.1.6.2); callers check the RangeError cases (division by zero,
// negative exponent) before calling.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/heap.h"

namespace snowman {

class BigInt final : public Cell {
 public:
  BigInt(bool negative, std::vector<uint32_t> magnitude);  // normalizes: no leading zero limbs, no -0

  bool isNegative() const { return negative_; }
  bool isZero() const { return magnitude_.empty(); }
  const std::vector<uint32_t>& magnitude() const { return magnitude_; }  // little-endian base 2^32

  static BigInt* fromInt64(int64_t value);
  static BigInt* fromUint64(uint64_t value);
  // NumberToBigInt: value must be an integral finite double.
  static BigInt* fromDouble(double value);
  // StringToBigInt (no sign handling beyond the spec's StringIntegerLiteral); nullopt if not a BigInt string.
  static std::optional<BigInt*> fromString(std::u16string_view text);
  // Digits in `radix` (2..36), optional leading '-'. Used by the parser's literals via decimal digits.
  static BigInt* fromDigits(std::string_view digits, int radix);

  std::u16string toString(int radix = 10) const;
  double toDouble() const;  // round-half-even, as Number(bigint)
  int64_t toInt64() const;   // BigInt.asIntN(64) wrap
  uint64_t toUint64() const;  // BigInt.asUintN(64) wrap

  static BigInt* unaryMinus(const BigInt* x);
  static BigInt* bitwiseNot(const BigInt* x);
  static BigInt* exponentiate(const BigInt* base, const BigInt* exponent);  // exponent >= 0
  static BigInt* multiply(const BigInt* x, const BigInt* y);
  static BigInt* divide(const BigInt* x, const BigInt* y);     // y != 0, truncates toward zero
  static BigInt* remainder(const BigInt* x, const BigInt* y);  // y != 0, sign of x
  static BigInt* add(const BigInt* x, const BigInt* y);
  static BigInt* subtract(const BigInt* x, const BigInt* y);
  static BigInt* leftShift(const BigInt* x, const BigInt* y);
  static BigInt* signedRightShift(const BigInt* x, const BigInt* y);
  static BigInt* bitwiseAnd(const BigInt* x, const BigInt* y);
  static BigInt* bitwiseXor(const BigInt* x, const BigInt* y);
  static BigInt* bitwiseOr(const BigInt* x, const BigInt* y);
  static BigInt* asIntN(uint64_t bits, const BigInt* x);
  static BigInt* asUintN(uint64_t bits, const BigInt* x);
  // -1, 0, 1
  static int compare(const BigInt* x, const BigInt* y);
  // Compare with a Number (NaN → nullopt).
  static std::optional<int> compareToNumber(const BigInt* x, double y);
  static bool equals(const BigInt* x, const BigInt* y) { return compare(x, y) == 0; }

 private:
  bool negative_;
  std::vector<uint32_t> magnitude_;
};

}  // namespace snowman
