// The Intl service objects whose internal slots other files need (String.prototype.localeCompare and friends,
// Number/BigInt/Date toLocaleString, DurationFormat on NumberFormat ...).
#pragma once

#include <string>

#include "builtins/intl/number_core.h"
#include "runtime/object.h"

namespace snowman::intl {

class CollatorObject : public Object {
 public:
  explicit CollatorObject(Object* prototype);
  std::string locale;       // [[Locale]]
  std::string usage;        // [[Usage]]
  std::string collation;    // [[Collation]]
  bool numeric = false;     // [[Numeric]]
  std::string caseFirst;    // [[CaseFirst]]
  std::string sensitivity;  // [[Sensitivity]]
  bool ignorePunctuation = false;
  Object* boundCompare = nullptr;  // [[BoundCompare]]
  void trace(Tracer& tracer) override;
};

Result<void> InitializeCollator(CollatorObject* collator, Value locales, Value options);
int CompareStrings(const CollatorObject& collator, std::u16string_view x, std::u16string_view y);

class NumberFormatObject : public Object {
 public:
  explicit NumberFormatObject(Object* prototype) : Object(prototype) {}
  std::string locale;           // [[Locale]]
  std::string dataLocale;       // [[DataLocale]]
  std::string numberingSystem;  // [[NumberingSystem]]
  std::string style = "decimal";
  std::string currency, currencyDisplay, currencySign;  // style "currency" only
  std::string unit, unitDisplay;                         // style "unit" only
  DigitOptions digits;
  std::string notation = "standard";
  std::string compactDisplay;  // notation "compact" only
  std::string useGrouping = "auto";  // "always", "auto", "min2" or "false"
  std::string signDisplay = "auto";
  Object* boundFormat = nullptr;  // [[BoundFormat]]
  void trace(Tracer& tracer) override;
};

// 15.1.2 InitializeNumberFormat ( numberFormat, locales, options )
Result<void> InitializeNumberFormat(NumberFormatObject* nf, Value locales, Value options);

struct NumberPart {
  const char16_t* type;  // "integer", "group", "literal", ...
  std::u16string value;
};
// 15.5.4 PartitionNumberPattern ( numberFormat, x )
std::vector<NumberPart> PartitionNumberPattern(const NumberFormatObject& nf, const Decimal& x);
// 15.5.5 FormatNumeric ( numberFormat, x )
std::u16string FormatNumeric(const NumberFormatObject& nf, const Decimal& x);
// The plural category (CLDR letter) of x as nf formats it (cardinal rules of nf's locale).
char numberFormatPluralCategory(const NumberFormatObject& nf, const Decimal& x);
// A NumberFormat created as by new Intl.NumberFormat(locales, options), in the current realm.
Result<NumberFormatObject*> createNumberFormat(Value locales, Value options);

// %Intl%: carries [[FallbackSymbol]] for the legacy constructor behaviour of NumberFormat and DateTimeFormat.
class IntlObject : public Object {
 public:
  using Object::Object;
  Symbol* fallbackSymbol = nullptr;
  void trace(Tracer& tracer) override;
};
Symbol* fallbackSymbol();

}  // namespace snowman::intl
