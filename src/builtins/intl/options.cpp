// Option processing: GetOptionsObject, CoerceOptionsToObject, GetOption, DefaultNumberOption, GetNumberOption
// (ECMA-402 9.2.11–9.2.17).
#include <cmath>

#include "builtins/intl/intl.h"

namespace snowman::intl {

Result<Object*> GetOptionsObject(Value options) {
  if (options.isUndefined()) return OrdinaryObjectCreate(nullptr);
  if (options.isObject()) return options.asObject();
  return ThrowTypeError("Intl: options must be an object");
}

Result<Object*> CoerceOptionsToObject(Value options) {
  if (options.isUndefined()) return OrdinaryObjectCreate(nullptr);
  return ToObject(options);
}

Result<std::optional<std::string>> GetStringOption(Object* options, std::u16string_view property,
                                                   std::initializer_list<std::string_view> values,
                                                   std::optional<std::string_view> fallback) {
  Value value = TRY(Get(options, PropertyKey(property)));
  if (value.isUndefined()) {
    if (fallback) return std::optional<std::string>(std::string(*fallback));
    return std::optional<std::string>();
  }
  String* s = TRY(ToString(value));
  std::u16string_view v = s->view();
  if (values.size() == 0) {
    // Any string; non-ASCII is kept as UTF-8 (callers that need the exact string re-read it).
    return std::optional<std::string>(s->toUtf8());
  }
  for (std::string_view allowed : values) {
    if (v.size() == allowed.size() && std::equal(v.begin(), v.end(), allowed.begin(),
                                                 [](char16_t a, char b) { return a == char16_t(uint8_t(b)); }))
      return std::optional<std::string>(std::string(allowed));
  }
  std::string message = "Value " + s->toUtf8() + " out of range for option " + utf16ToUtf8(property);
  return ThrowRangeError(message);
}

Result<std::optional<bool>> GetBooleanOption(Object* options, std::u16string_view property) {
  Value value = TRY(Get(options, PropertyKey(property)));
  if (value.isUndefined()) return std::optional<bool>();
  return std::optional<bool>(ToBoolean(value));
}

Result<std::optional<double>> DefaultNumberOption(Value value, double minimum, double maximum,
                                                 std::optional<double> fallback) {
  if (value.isUndefined()) return fallback;
  double d = TRY(ToNumber(value));
  if (std::isnan(d) || d < minimum || d > maximum) return ThrowRangeError("Intl: number option out of range");
  return std::optional<double>(std::floor(d));
}

Result<std::optional<double>> GetNumberOption(Object* options, std::u16string_view property, double minimum,
                                             double maximum, std::optional<double> fallback) {
  Value value = TRY(Get(options, PropertyKey(property)));
  return DefaultNumberOption(value, minimum, maximum, fallback);
}

Result<std::string> GetLocaleMatcher(Object* options) {
  std::optional<std::string> m = TRY(GetStringOption(options, u"localeMatcher", {"lookup", "best fit"}, "best fit"));
  return *m;
}

Value optionalString(const std::optional<std::string>& s) { return s ? jsString(*s) : Value::undefined(); }

void addProperty(Object* o, std::u16string_view key, Value value) {
  Result<void> r = CreateDataPropertyOrThrow(o, PropertyKey(key), value);
  (void)r.isThrow();
}

}  // namespace snowman::intl
