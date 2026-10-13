// Intl.Locale objects (ECMA-402 14): the internal slots, shared with CanonicalizeLocaleList.
#pragma once

#include <optional>
#include <string>

#include "runtime/object.h"

namespace snowman::intl {

class LocaleObject : public Object {
 public:
  using Object::Object;
  std::string locale;                          // [[Locale]] (canonical)
  std::optional<std::string> calendar;         // [[Calendar]]
  std::optional<std::string> collation;        // [[Collation]]
  std::optional<std::string> firstDayOfWeek;   // [[FirstDayOfWeek]]
  std::optional<std::string> hourCycle;        // [[HourCycle]]
  std::optional<std::string> caseFirst;        // [[CaseFirst]]
  bool numeric = false;                        // [[Numeric]]
  std::optional<std::string> numberingSystem;  // [[NumberingSystem]]
};

}  // namespace snowman::intl
