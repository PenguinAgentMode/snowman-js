// Locale lists and negotiation: CanonicalizeLocaleList, BestAvailableLocale, LookupMatcher, BestFitMatcher,
// ResolveLocale, SupportedLocales (ECMA-402 9.2).
#include <algorithm>

#include "builtins/intl/intl.h"
#include "builtins/intl/locale_object.h"

namespace snowman::intl {

const std::string& DefaultLocale() {
  static const std::string locale = "en-US";
  return locale;
}

// 9.2.1 CanonicalizeLocaleList ( locales )
Result<std::vector<std::string>> CanonicalizeLocaleList(Value locales) {
  std::vector<std::string> seen;
  if (locales.isUndefined()) return seen;
  Object* o;
  if (locales.isString() || (locales.isObject() && locales.asObject()->is<LocaleObject>())) {
    o = CreateArrayFromList({locales});
  } else {
    o = TRY(ToObject(locales));
  }
  double len = TRY(LengthOfArrayLike(o));
  for (double k = 0; k < len; k++) {
    PropertyKey pk = builtins::indexKey(k);
    bool present = TRY(HasProperty(o, pk));
    if (!present) continue;
    Value kValue = TRY(Get(o, pk));
    if (!kValue.isString() && !kValue.isObject()) return ThrowTypeError("Intl: locale must be a string or an object");
    std::string tag;
    if (kValue.isObject() && kValue.asObject()->is<LocaleObject>()) {
      tag = kValue.asObject()->as<LocaleObject>()->locale;
    } else {
      String* s = TRY(ToString(kValue));
      std::optional<LanguageTag> parsed = ParseLanguageTag(s->view());
      if (!parsed) return ThrowRangeError("Incorrect locale information provided: " + s->toUtf8());
      CanonicalizeUnicodeLocaleId(*parsed);
      tag = parsed->toString();
    }
    if (std::find(seen.begin(), seen.end(), tag) == seen.end()) seen.push_back(std::move(tag));
  }
  return seen;
}

// 9.2.2 BestAvailableLocale ( availableLocales, locale )
std::optional<std::string> BestAvailableLocale(std::span<const std::string> available, std::string_view locale) {
  std::string candidate(locale);
  while (true) {
    if (std::binary_search(available.begin(), available.end(), candidate)) return candidate;
    size_t pos = candidate.rfind('-');
    if (pos == std::string::npos) return std::nullopt;
    if (pos >= 2 && candidate[pos - 2] == '-') pos -= 2;
    candidate.resize(pos);
  }
}

namespace {

struct MatcherResult {
  std::string locale;
  std::string extension;  // "-u-..." or ""
};

// 9.2.3 LookupMatcher ( availableLocales, requestedLocales ); 9.2.4 BestFitMatcher is the same here.
MatcherResult LookupMatcher(std::span<const std::string> available, const std::vector<std::string>& requested) {
  for (const std::string& locale : requested) {
    std::string extension;
    std::string noExtensionsLocale = removeUnicodeExtensions(locale, &extension);
    if (std::optional<std::string> availableLocale = BestAvailableLocale(available, noExtensionsLocale))
      return {*availableLocale, extension};
  }
  return {DefaultLocale(), {}};
}

}  // namespace

const LocaleOption* LocaleOptions::find(std::string_view key) const {
  for (const auto& [k, v] : keys)
    if (k == key) return &v;
  return nullptr;
}

const std::string& ResolvedLocale::get(std::string_view key) const {
  static const std::string empty;
  for (const auto& [k, v] : values)
    if (k == key) return v;
  return empty;
}

// 9.2.7 ResolveLocale ( availableLocales, requestedLocales, options, relevantExtensionKeys, localeData )
ResolvedLocale ResolveLocale(const ServiceData& data, const std::vector<std::string>& requestedLocales,
                             const LocaleOptions& options, std::span<const char* const> relevantExtensionKeys) {
  MatcherResult r = LookupMatcher(data.availableLocales(), requestedLocales);
  ResolvedLocale result;
  result.dataLocale = r.locale;
  std::vector<Keyword> keywords;
  if (!r.extension.empty()) {
    if (std::optional<LanguageTag> t = ParseLanguageTag(std::string_view("und" + r.extension))) keywords = t->keywords;
  }
  std::vector<Keyword> supportedKeywords;
  for (const char* keyName : relevantExtensionKeys) {
    std::string key(keyName);
    std::vector<std::string> keyLocaleData = data.keyValues(r.locale, key);
    std::string value = keyLocaleData.empty() ? std::string() : keyLocaleData[0];
    std::optional<Keyword> supportedKeyword;
    auto contains = [&](const std::string& v) {
      return std::find(keyLocaleData.begin(), keyLocaleData.end(), v) != keyLocaleData.end();
    };
    for (const Keyword& entry : keywords) {
      if (entry.key != key) continue;
      const std::string& requestedValue = entry.value;
      if (!requestedValue.empty()) {
        if (contains(requestedValue)) {
          value = requestedValue;
          supportedKeyword = Keyword{key, value};
        }
      } else if (contains("true")) {
        value = "true";
        supportedKeyword = Keyword{key, ""};
      }
      break;
    }
    if (const LocaleOption* opt = options.find(key)) {
      if (opt->kind != LocaleOption::Kind::Undefined) {
        // A null option (DateTimeFormat's hc when hour12 is given) is "" here: it matches a null in the locale data.
        std::string optionsValue;
        if (opt->kind == LocaleOption::Kind::String) {
          optionsValue = CanonicalizeUValue(key, asciiLower(opt->value));
          if (optionsValue.empty()) optionsValue = "true";
        }
        if (optionsValue != value && contains(optionsValue)) {
          value = optionsValue;
          supportedKeyword.reset();
        }
      }
    }
    if (supportedKeyword) supportedKeywords.push_back(*supportedKeyword);
    result.values.emplace_back(key, value);
  }
  std::string foundLocale = r.locale;
  if (!supportedKeywords.empty()) {
    // InsertUnicodeExtensionAndCanonicalize ( locale, attributes, keywords )
    if (std::optional<LanguageTag> t = ParseLanguageTag(std::string_view(foundLocale))) {
      t->hasUnicodeExtension = true;
      t->keywords = supportedKeywords;
      CanonicalizeUnicodeLocaleId(*t);
      foundLocale = t->toString();
    }
  }
  result.locale = foundLocale;
  return result;
}

// 9.2.8 LookupSupportedLocales / 9.2.9 BestFitSupportedLocales: the requested locales whose extension-free form has
// a best available locale.
// 9.2.10 SupportedLocales ( availableLocales, requestedLocales, options )
Result<Value> SupportedLocales(const ServiceData& data, const std::vector<std::string>& requestedLocales, Value options) {
  Object* opts = TRY(CoerceOptionsToObject(options));
  TRY(GetLocaleMatcher(opts));
  ValueVector subset;
  std::span<const std::string> available = data.availableLocales();
  for (const std::string& locale : requestedLocales) {
    if (BestAvailableLocale(available, removeUnicodeExtensions(locale))) subset.push_back(jsString(locale));
  }
  return Value(CreateArrayFromList(subset));
}

Result<Value> supportedLocalesOf(const ServiceData& data, NativeCall& call) {
  std::vector<std::string> requested = TRY(CanonicalizeLocaleList(call.args[0]));
  return SupportedLocales(data, requested, call.args[1]);
}

}  // namespace snowman::intl
