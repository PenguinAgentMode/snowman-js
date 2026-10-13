// ECMA-402 (Intl): the shared abstract operations of src/builtins/intl/. Language tags (UTS 35) are ASCII and
// handled as std::string; values exposed to JS are converted at the edges.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "builtins/util.h"

namespace snowman::intl {

using builtins::boolean;
using builtins::num;
using builtins::str;

// ---- ASCII helpers ----
std::string asciiLower(std::string_view s);
std::string asciiUpper(std::string_view s);
// The string as ASCII, or nullopt if it has a non-ASCII code unit.
std::optional<std::string> toAscii(std::u16string_view s);
inline Value jsString(std::string_view ascii) { return Value(newString(builtins::toU16(ascii))); }
String* jsStr(std::string_view ascii);
std::vector<std::string> split(std::string_view s, char sep);
std::string join(const std::vector<std::string>& parts, std::string_view sep);

// ---- Language tags (locale_id.cpp): ECMA-402 6.2, UTS 35 Part 1 §3 and Annex C ----

struct LanguageId {
  std::string language;               // lowercase
  std::string script;                 // "" or Titlecase
  std::string region;                 // "" or uppercase
  std::vector<std::string> variants;  // lowercase
  std::string toString() const;
};

// A u keyword or t field; value "" is "true" (u) and never empty for t.
struct Keyword {
  std::string key;
  std::string value;
};

struct LanguageTag {
  LanguageId id;
  bool hasUnicodeExtension = false;
  std::vector<std::string> attributes;
  std::vector<Keyword> keywords;
  bool hasTransformedExtension = false;
  std::optional<LanguageId> tlang;
  std::vector<Keyword> tfields;
  std::vector<std::pair<char, std::string>> otherExtensions;  // singleton, "sub-tags"
  std::string privateUse;                                     // "" or the subtags after "x-"
  // The tag, with extensions ordered by singleton (canonical order) and the private use last.
  std::string toString() const;
  std::string baseName() const { return id.toString(); }
  // The value of a u keyword, or nullopt.
  const std::string* keyword(std::string_view key) const;
};

// IsStructurallyValidLanguageTag + parse, case-normalised (nullopt when not structurally valid).
std::optional<LanguageTag> ParseLanguageTag(std::string_view tag);
std::optional<LanguageTag> ParseLanguageTag(std::u16string_view tag);
// CanonicalizeUnicodeLocaleId (6.2.3): alias replacement, sorting, value canonicalization; in place.
void CanonicalizeUnicodeLocaleId(LanguageTag& tag);
// Parse + canonicalize; nullopt when not structurally valid.
std::optional<std::string> CanonicalizeLanguageTag(std::string_view tag);
// CanonicalizeUValue (6.2.4): the canonical form of a u keyword value (lowercase input).
std::string CanonicalizeUValue(std::string_view key, std::string_view value);
// Replace the aliases of a unicode_language_id (UTS 35 Annex C), sorting its variants.
void ReplaceLanguageIdAliases(LanguageId& id);
// UTS 35 Add Likely Subtags / Remove Likely Subtags (favor region). False when there is no likely-subtags match
// (the id is left as it was).
bool AddLikelySubtags(LanguageId& id);
bool RemoveLikelySubtags(LanguageId& id);

// Subtag shapes (UTS 35 unicode_language_id), case-insensitive.
bool isUnicodeLanguageSubtag(std::string_view s);
bool isUnicodeScriptSubtag(std::string_view s);
bool isUnicodeRegionSubtag(std::string_view s);
bool isUnicodeVariantSubtag(std::string_view s);
// type = alphanum{3,8} (sep alphanum{3,8})*
bool isUnicodeLocaleType(std::string_view s);
// The "-u-..." sequence of a canonical tag removed (for the matchers), and the sequence itself.
std::string removeUnicodeExtensions(std::string_view tag, std::string* extension = nullptr);

// ---- Locale lists and resolution (locale_resolution.cpp, ECMA-402 9.2) ----

// 9.2.1 CanonicalizeLocaleList ( locales )
Result<std::vector<std::string>> CanonicalizeLocaleList(Value locales);
// The host's default locale ("en-US").
const std::string& DefaultLocale();

// A service's locale data: its available locales (canonical, no extensions) and, for each relevant extension key,
// the supported values for a data locale (the default first).
struct ServiceData {
  std::span<const std::string> (*availableLocales)();
  std::vector<std::string> (*keyValues)(const std::string& dataLocale, std::string_view key);
};

// An option of the ResolveLocale options record: undefined, null or a String.
struct LocaleOption {
  enum class Kind : uint8_t { Undefined, Null, String } kind = Kind::Undefined;
  std::string value;
  static LocaleOption string(std::string s) { return {Kind::String, std::move(s)}; }
  static LocaleOption null() { return {Kind::Null, {}}; }
};
struct LocaleOptions {
  std::string localeMatcher = "best fit";
  std::vector<std::pair<std::string, LocaleOption>> keys;  // [[ca]], [[nu]], ... in relevantExtensionKeys terms
  const LocaleOption* find(std::string_view key) const;
};
struct ResolvedLocale {
  std::string locale;      // [[Locale]], with the supported keywords
  std::string dataLocale;  // [[DataLocale]]: the matched available locale
  std::vector<std::pair<std::string, std::string>> values;  // [[<key>]] for each relevant key
  const std::string& get(std::string_view key) const;
};
// 9.2.7 ResolveLocale ( availableLocales, requestedLocales, options, relevantExtensionKeys, localeData )
ResolvedLocale ResolveLocale(const ServiceData& data, const std::vector<std::string>& requestedLocales,
                             const LocaleOptions& options, std::span<const char* const> relevantExtensionKeys);
// 9.2.2 BestAvailableLocale
std::optional<std::string> BestAvailableLocale(std::span<const std::string> available, std::string_view locale);
// 9.2.10 SupportedLocales ( availableLocales, requestedLocales, options ): an Array.
Result<Value> SupportedLocales(const ServiceData& data, const std::vector<std::string>& requestedLocales, Value options);
// The common supportedLocalesOf ( locales [ , options ] ) body.
Result<Value> supportedLocalesOf(const ServiceData& data, NativeCall& call);
// The locales every service knows (canonical, sorted).
std::span<const std::string> commonLocales();

// ---- Options (options.cpp, ECMA-402 9.2.11–9.2.17) ----

// GetOptionsObject: undefined → a null-prototype object; an object; else TypeError.
Result<Object*> GetOptionsObject(Value options);
// CoerceOptionsToObject: undefined → a null-prototype object; else ToObject.
Result<Object*> CoerceOptionsToObject(Value options);
// GetOption ( options, property, "string", values, default ): nullopt for undefined with no default.
Result<std::optional<std::string>> GetStringOption(Object* options, std::u16string_view property,
                                                   std::initializer_list<std::string_view> values,
                                                   std::optional<std::string_view> fallback = std::nullopt);
// GetOption ( options, property, "boolean", empty, default ): nullopt for undefined.
Result<std::optional<bool>> GetBooleanOption(Object* options, std::u16string_view property);
// DefaultNumberOption / GetNumberOption: nullopt for undefined with no fallback.
Result<std::optional<double>> DefaultNumberOption(Value value, double minimum, double maximum,
                                                 std::optional<double> fallback);
Result<std::optional<double>> GetNumberOption(Object* options, std::u16string_view property, double minimum,
                                             double maximum, std::optional<double> fallback);
// The localeMatcher option ("lookup" / "best fit").
Result<std::string> GetLocaleMatcher(Object* options);
// A string option's value as a JS string (undefined for nullopt).
Value optionalString(const std::optional<std::string>& s);

// ---- Objects ----

// CreateDataPropertyOrThrow(o, key, value) for resolvedOptions(): never fails on an ordinary object.
void addProperty(Object* o, std::u16string_view key, Value value);
// The receiver as T, or a TypeError naming the method.
template <class T>
Result<T*> thisIntlObject(Value thisValue, const char* method) {
  if (thisValue.isObject()) {
    if (T* t = thisValue.asObject()->as<T>()) return t;
  }
  return ThrowTypeError(std::string(method) + " called on incompatible receiver");
}

// ---- Locale data (locale_data.cpp; generated tables in locale_data.inc) ----
namespace data {

// The available locales (canonical, sorted).
std::span<const std::string> locales();
// The locale's parent as BestAvailableLocale truncates it ("" at the root).
std::string parentLocale(std::string_view locale);
std::string defaultNumberingSystem(std::string_view locale);
std::string defaultCalendar(std::string_view locale);
std::string defaultHourCycle(std::string_view locale);
// The collations a locale supports besides "default" (sorted).
std::vector<std::string> collations(std::string_view locale);
// The UTF-16 digits 0-9 of a numbering system with a simple digit mapping, or nullptr.
const char16_t* numberingSystemDigits(std::string_view numberingSystem);
// Every numbering system with a simple digit mapping (sorted).
std::vector<std::string> numberingSystems();
bool isSupportedNumberingSystem(std::string_view numberingSystem);
// The supportedValuesOf lists (sorted).
std::vector<std::string> calendars();
std::vector<std::string> collationTypes();
std::vector<std::string> currencies();
std::vector<std::string> units();
bool isSanctionedSimpleUnit(std::string_view unit);
// Time zones: the primary identifier for a named time zone (case-insensitive), the primary identifiers (sorted),
// and those of a region (sorted).
std::optional<std::string> primaryTimeZone(std::string_view name);
std::vector<std::string> primaryTimeZones();
std::vector<std::string> regionTimeZones(std::string_view region);
// Locale info by region (falling back to "001"): calendars and hour cycles by preference, week data.
std::vector<std::string> regionCalendars(std::string_view region);
std::vector<std::string> regionHourCycles(std::string_view region);
struct WeekData {
  int firstDay, minimalDays, weekendStart, weekendEnd;  // 1 = Monday ... 7 = Sunday
};
WeekData regionWeek(std::string_view region);
bool isRtlScript(std::string_view script);

}  // namespace data

// ---- Installation (intl_object.cpp creates %Intl% and calls these) ----
void installCollator(Realm& realm, Object* intl);
void installDateTimeFormat(Realm& realm, Object* intl);
void installDisplayNames(Realm& realm, Object* intl);
void installDurationFormat(Realm& realm, Object* intl);
void installListFormat(Realm& realm, Object* intl);
void installLocale(Realm& realm, Object* intl);
void installNumberFormat(Realm& realm, Object* intl);
void installPluralRules(Realm& realm, Object* intl);
void installRelativeTimeFormat(Realm& realm, Object* intl);
void installSegmenter(Realm& realm, Object* intl);

// A constructor/prototype pair as a property of %Intl% (writable, non-enumerable, configurable), with
// prototype[@@toStringTag] = "Intl.<name>".
BuiltinFunction* defineIntlConstructor(Realm& realm, Object* intl, Intrinsic constructorId, NativeFn fn, int length,
                                       std::u16string_view name, Intrinsic prototypeId, Object** prototypeOut);

}  // namespace snowman::intl
