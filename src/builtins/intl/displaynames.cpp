// Intl.DisplayNames (ECMA-402 12): English names from display_data.inc (other locales use them too), language names
// composed as "language (script, region, variants)" unless a dialect name applies.
#include <algorithm>

#include "builtins/intl/intl.h"

namespace snowman::intl {

namespace {

struct DataEntry {
  const char* key;
  const char16_t* value;
};
#include "builtins/intl/display_data.inc"

class DisplayNamesObject : public Object {
 public:
  using Object::Object;
  std::string locale, style, type, fallback, languageDisplay;
};

std::vector<std::string> noKeys(const std::string&, std::string_view) { return {}; }
const ServiceData kDisplayNamesData{data::locales, noKeys};

const char16_t* lookupName(const std::string& type, const std::string& style, const std::string& code) {
  auto find = [](const std::string& key) -> const char16_t* {
    const DataEntry* end = std::end(kDisplayNames);
    const DataEntry* it = std::lower_bound(std::begin(kDisplayNames), end, key,
                                           [](const DataEntry& e, const std::string& k) { return std::string_view(e.key) < k; });
    return it != end && key == it->key ? it->value : nullptr;
  };
  if (style == "narrow")
    if (const char16_t* v = find(type + "/narrow/" + code)) return v;
  if (style != "long")
    if (const char16_t* v = find(type + "/short/" + code)) return v;
  return find(type + "/long/" + code);
}

// The display name of a language id; nullopt when the language has no name and the fallback is "none".
std::optional<std::u16string> languageName(const DisplayNamesObject& dn, const LanguageId& id) {
  std::u16string base;
  std::vector<std::u16string> qualifiers;
  bool usedScript = false, usedRegion = false;
  if (dn.languageDisplay == "dialect") {
    // The longest dialect name among language-script-region, language-region, language-script.
    std::vector<LanguageId> candidates;
    if (!id.script.empty() && !id.region.empty()) candidates.push_back({id.language, id.script, id.region, {}});
    if (!id.region.empty()) candidates.push_back({id.language, "", id.region, {}});
    if (!id.script.empty()) candidates.push_back({id.language, id.script, "", {}});
    for (const LanguageId& c : candidates) {
      if (const char16_t* v = lookupName("dialect", dn.style, c.toString())) {
        base = v;
        usedScript = !c.script.empty();
        usedRegion = !c.region.empty();
        break;
      }
    }
  }
  if (base.empty()) {
    if (const char16_t* v = lookupName("language", dn.style, id.language)) base = v;
    else if (dn.fallback == "none") return std::nullopt;
    else base = builtins::toU16(id.language);
  }
  if (!id.script.empty() && !usedScript) {
    const char16_t* v = lookupName("script", dn.style, id.script);
    qualifiers.push_back(v ? std::u16string(v) : builtins::toU16(id.script));
  }
  if (!id.region.empty() && !usedRegion) {
    const char16_t* v = lookupName("region", dn.style, id.region);
    qualifiers.push_back(v ? std::u16string(v) : builtins::toU16(id.region));
  }
  for (const std::string& variant : id.variants) qualifiers.push_back(builtins::toU16(variant));
  if (qualifiers.empty()) return base;
  std::u16string q;
  for (size_t i = 0; i < qualifiers.size(); i++) {
    if (i) q += kLocaleSeparator;
    q += qualifiers[i];
  }
  std::u16string out(kLocalePattern);
  out.replace(out.find(u"{0}"), 3, base);
  out.replace(out.find(u"{1}"), 3, q);
  return out;
}

bool isDateTimeField(const std::string& code) {
  static const char* const fields[] = {"era", "year", "quarter", "month", "weekOfYear", "weekday", "day", "dayPeriod",
                                       "hour", "minute", "second", "timeZoneName"};
  return std::find_if(std::begin(fields), std::end(fields), [&](const char* f) { return code == f; }) != std::end(fields);
}

// 12.1.1 Intl.DisplayNames ( locales, options )
Result<Value> DisplayNames_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Intl.DisplayNames requires 'new'");
  DisplayNamesObject* dn = TRY(OrdinaryCreateFromConstructor<DisplayNamesObject>(call.newTarget, Intrinsic::IntlDisplayNamesPrototype));
  std::vector<std::string> requested = TRY(CanonicalizeLocaleList(call.args[0]));
  if (call.args[1].isUndefined()) return ThrowTypeError("Intl.DisplayNames: options are required");
  Object* options = TRY(GetOptionsObject(call.args[1]));
  LocaleOptions opt;
  opt.localeMatcher = TRY(GetLocaleMatcher(options));
  ResolvedLocale r = ResolveLocale(kDisplayNamesData, requested, opt, {});
  dn->style = *TRY(GetStringOption(options, u"style", {"narrow", "short", "long"}, "long"));
  std::optional<std::string> type =
      TRY(GetStringOption(options, u"type", {"language", "region", "script", "currency", "calendar", "dateTimeField"}));
  if (!type) return ThrowTypeError("Intl.DisplayNames: type is required");
  dn->type = *type;
  dn->fallback = *TRY(GetStringOption(options, u"fallback", {"code", "none"}, "code"));
  dn->locale = r.locale;
  std::string languageDisplay = *TRY(GetStringOption(options, u"languageDisplay", {"dialect", "standard"}, "dialect"));
  if (dn->type == "language") dn->languageDisplay = languageDisplay;
  return Value(dn);
}

Result<Value> DisplayNames_supportedLocalesOf(NativeCall& call) { return supportedLocalesOf(kDisplayNamesData, call); }

// 12.3.3 Intl.DisplayNames.prototype.of ( code )
Result<Value> DisplayNamesPrototype_of(NativeCall& call) {
  DisplayNamesObject* dn = TRY(thisIntlObject<DisplayNamesObject>(call.thisValue, "Intl.DisplayNames.prototype.of"));
  String* codeString = TRY(ToString(call.args[0]));
  std::optional<std::string> ascii = toAscii(codeString->view());
  auto invalid = [&]() { return ThrowRangeError("Intl.DisplayNames: invalid code " + codeString->toUtf8()); };
  if (!ascii) return invalid();
  std::string code = *ascii;
  // CanonicalCodeForDisplayNames ( type, code )
  const char16_t* name = nullptr;
  if (dn->type == "language") {
    std::optional<LanguageTag> tag = ParseLanguageTag(std::string_view(code));
    if (!tag || tag->hasUnicodeExtension || tag->hasTransformedExtension || !tag->otherExtensions.empty() || !tag->privateUse.empty())
      return invalid();
    CanonicalizeUnicodeLocaleId(*tag);
    std::optional<std::u16string> n = languageName(*dn, tag->id);
    if (!n) return Value::undefined();
    return Value(newString(*n));
  } else if (dn->type == "region") {
    if (!isUnicodeRegionSubtag(code)) return invalid();
    code = asciiUpper(code);
  } else if (dn->type == "script") {
    if (!isUnicodeScriptSubtag(code)) return invalid();
    code = asciiLower(code);
    code[0] = char(code[0] - 32);
  } else if (dn->type == "calendar") {
    if (!isUnicodeLocaleType(code) || code.find('_') != std::string::npos) return invalid();
    code = asciiLower(code);
    if (code == "gregorian") code = "gregory";
  } else if (dn->type == "dateTimeField") {
    if (!isDateTimeField(code)) return invalid();
  } else {  // currency
    if (code.size() != 3 || !std::all_of(code.begin(), code.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }))
      return invalid();
    code = asciiUpper(code);
  }
  name = lookupName(dn->type, dn->style, code);
  if (name) return Value(newString(std::u16string_view(name)));
  if (dn->fallback == "code") return jsString(code);
  return Value::undefined();
}

Result<Value> DisplayNamesPrototype_resolvedOptions(NativeCall& call) {
  DisplayNamesObject* dn = TRY(thisIntlObject<DisplayNamesObject>(call.thisValue, "Intl.DisplayNames.prototype.resolvedOptions"));
  Object* o = builtins::newPlainObject();
  addProperty(o, u"locale", jsString(dn->locale));
  addProperty(o, u"style", jsString(dn->style));
  addProperty(o, u"type", jsString(dn->type));
  addProperty(o, u"fallback", jsString(dn->fallback));
  if (dn->type == "language") addProperty(o, u"languageDisplay", jsString(dn->languageDisplay));
  return Value(o);
}

}  // namespace

void installDisplayNames(Realm& realm, Object* intl) {
  Object* proto = nullptr;
  BuiltinFunction* ctor = defineIntlConstructor(realm, intl, Intrinsic::IntlDisplayNames, DisplayNames_construct, 2,
                                                u"DisplayNames", Intrinsic::IntlDisplayNamesPrototype, &proto);
  defineFunction(realm, ctor, PropertyKey(u"supportedLocalesOf"), 1, DisplayNames_supportedLocalesOf);
  defineFunction(realm, proto, PropertyKey(u"of"), 1, DisplayNamesPrototype_of);
  defineFunction(realm, proto, PropertyKey(u"resolvedOptions"), 0, DisplayNamesPrototype_resolvedOptions);
}

}  // namespace snowman::intl
