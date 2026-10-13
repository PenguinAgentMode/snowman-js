// Intl numbers: Intl mathematical values (exact decimals), the digit options and rounding of NumberFormat and
// PluralRules (ECMA-402 15.5), and plural rule evaluation (UTS 35 plural rules over CLDR data).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "builtins/intl/intl.h"

namespace snowman::intl {

// An Intl mathematical value: a finite decimal digits × 10^exponent (digits without leading or trailing zeros, ""
// for zero), or not-a-number / positive-infinity / negative-infinity. A negative zero is Finite, negative, "".
struct Decimal {
  enum class Kind : uint8_t { Finite, NaN, PositiveInfinity, NegativeInfinity };
  Kind kind = Kind::Finite;
  bool negative = false;
  std::string digits;
  int64_t exponent = 0;

  bool isFinite() const { return kind == Kind::Finite; }
  bool isZero() const { return kind == Kind::Finite && digits.empty(); }
  bool isNaN() const { return kind == Kind::NaN; }
  // floor(log10(|x|)) of a finite nonzero value.
  int64_t magnitude() const { return int64_t(digits.size()) - 1 + exponent; }
  // x × 10^n
  Decimal scaled(int64_t n) const {
    Decimal d = *this;
    if (!d.digits.empty()) d.exponent += n;
    return d;
  }
  Decimal abs() const {
    Decimal d = *this;
    d.negative = false;
    return d;
  }
  // The value as a double (correctly rounded).
  double toDouble() const;

  static Decimal fromDouble(double d);
  static Decimal fromBigInt(BigInt* b);
  static Decimal fromIntegerString(std::string_view digits, bool negative);
  static Decimal nan() { return {Kind::NaN, false, {}, 0}; }
  void normalize();
};

// -1, 0, 1 comparing finite values (NaN/infinities by their order).
int compareDecimal(const Decimal& a, const Decimal& b);

// 15.5.16 ToIntlMathematicalValue ( value )
Result<Decimal> ToIntlMathematicalValue(Value value);
// The StringIntlMV of a string (StringNumericLiteral), not-a-number when it does not parse.
Decimal parseIntlMathematicalValue(std::u16string_view text);

enum class RoundingType : uint8_t { FractionDigits, SignificantDigits, MorePrecision, LessPrecision };

// The digit and rounding slots NumberFormat and PluralRules share.
struct DigitOptions {
  int minimumIntegerDigits = 1;
  int minimumFractionDigits = 0, maximumFractionDigits = 3;
  int minimumSignificantDigits = 1, maximumSignificantDigits = 21;
  bool hasFractionDigits = true, hasSignificantDigits = false;  // whether resolvedOptions reports them
  int roundingIncrement = 1;
  std::string roundingMode = "halfExpand";
  RoundingType roundingType = RoundingType::FractionDigits;
  std::string computedRoundingPriority = "auto";
  std::string trailingZeroDisplay = "auto";
};

// 15.1.3 SetNumberFormatDigitOptions ( intlObj, options, mnfdDefault, mxfdDefault, notation )
Result<void> SetNumberFormatDigitOptions(DigitOptions& d, Object* options, int mnfdDefault, int mxfdDefault,
                                         std::string_view notation);

// FormatNumericToString's result: the rounded value (signed) and its integer and fraction digits (ASCII).
struct RawFormat {
  Decimal rounded;
  std::string integer;
  std::string fraction;
};
// 15.5.3 FormatNumericToString ( intlObject, x ) for a finite x.
RawFormat FormatNumericToString(const DigitOptions& d, const Decimal& x);

// Plural rules (cardinal or ordinal) of a locale; categories as the CLDR letters z o t f m x.
struct PluralRuleSet {
  std::vector<std::pair<char, std::string>> rules;  // conditions, "other" excluded
  std::string ranges;                               // start/end/result triples
};
const PluralRuleSet& pluralRulesFor(std::string_view locale, bool ordinal);
// The category of a formatted decimal ("1.50": integer "1", fraction "50"); exponent is the compact exponent.
char pluralCategory(const PluralRuleSet& rules, const std::string& integer, const std::string& fraction, int exponent = 0);
std::string_view categoryName(char letter);
char categoryLetter(std::string_view name);
// The categories a rule set can produce, in CLDR order (other last).
std::vector<char> pluralCategories(const PluralRuleSet& rules);
// The category of a range start..end.
char pluralRangeCategory(const PluralRuleSet& rules, char start, char end);

}  // namespace snowman::intl
