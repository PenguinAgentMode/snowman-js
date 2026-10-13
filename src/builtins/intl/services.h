// The Intl service objects whose internal slots other files need (String.prototype.localeCompare and friends,
// Number/BigInt/Date toLocaleString, DurationFormat on NumberFormat ...).
#pragma once

#include <string>

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

}  // namespace snowman::intl
