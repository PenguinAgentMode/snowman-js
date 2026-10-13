// Intl.Locale (ECMA-402 14) and the Locale Info methods (getCalendars, getWeekInfo, ...).
#include <algorithm>

#include "builtins/intl/intl.h"
#include "builtins/intl/locale_object.h"

namespace snowman::intl {

namespace {

// WeekdayToString ( fw )
std::string weekdayToString(const std::string& fw) {
  static const char* const names[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat", "sun"};
  if (fw.size() == 1 && fw[0] >= '0' && fw[0] <= '7') return names[fw[0] - '0'];
  return fw;
}

// A string option that must match the `type` production (calendar, collation, numberingSystem, ...).
Result<std::optional<std::string>> typeOption(Object* options, std::u16string_view name) {
  std::optional<std::string> v = TRY(GetStringOption(options, name, {}));
  if (v && !isUnicodeLocaleType(*v)) return ThrowRangeError("Intl.Locale: invalid " + utf16ToUtf8(name) + " " + *v);
  return v;
}

// UpdateLanguageId ( tag, options ): applies the language, script, region and variants options.
Result<void> UpdateLanguageId(LanguageTag& tag, Object* options) {
  LanguageId& id = tag.id;
  std::optional<std::string> language = TRY(GetStringOption(options, u"language", {}, id.language));
  if (!isUnicodeLanguageSubtag(*language)) return ThrowRangeError("Intl.Locale: invalid language " + *language);
  std::optional<std::string_view> scriptDefault;
  if (!id.script.empty()) scriptDefault = id.script;
  std::optional<std::string> script = TRY(GetStringOption(options, u"script", {}, scriptDefault));
  if (script && !isUnicodeScriptSubtag(*script)) return ThrowRangeError("Intl.Locale: invalid script " + *script);
  std::optional<std::string_view> regionDefault;
  if (!id.region.empty()) regionDefault = id.region;
  std::optional<std::string> region = TRY(GetStringOption(options, u"region", {}, regionDefault));
  if (region && !isUnicodeRegionSubtag(*region)) return ThrowRangeError("Intl.Locale: invalid region " + *region);
  std::string variantsDefault = join(id.variants, "-");
  std::optional<std::string_view> variantsFallback;
  if (!id.variants.empty()) variantsFallback = variantsDefault;
  std::optional<std::string> variants = TRY(GetStringOption(options, u"variants", {}, variantsFallback));
  std::vector<std::string> variantList;
  if (variants) {
    if (variants->empty()) return ThrowRangeError("Intl.Locale: invalid variants");
    for (const std::string& v : split(asciiLower(*variants), '-')) {
      if (!isUnicodeVariantSubtag(v)) return ThrowRangeError("Intl.Locale: invalid variants " + *variants);
      if (std::find(variantList.begin(), variantList.end(), v) != variantList.end())
        return ThrowRangeError("Intl.Locale: duplicate variant " + v);
      variantList.push_back(v);
    }
  }
  id.language = asciiLower(*language);
  id.script.clear();
  if (script) {
    id.script = asciiLower(*script);
    id.script[0] = char(id.script[0] - 32);
  }
  id.region = region ? asciiUpper(*region) : std::string();
  id.variants = std::move(variantList);
  return {};
}

std::optional<std::string> keywordValue(const LanguageTag& tag, std::string_view key) {
  if (const std::string* v = tag.keyword(key)) return *v;
  return std::nullopt;
}

// 14.1.1 Intl.Locale ( tag [ , options ] )
Result<Value> Locale_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Intl.Locale requires 'new'");
  LocaleObject* locale = TRY(OrdinaryCreateFromConstructor<LocaleObject>(call.newTarget, Intrinsic::IntlLocalePrototype));
  Value tagValue = call.args[0];
  if (!tagValue.isString() && !tagValue.isObject()) return ThrowTypeError("Intl.Locale: tag must be a string or an object");
  std::string tagString;
  if (tagValue.isObject() && tagValue.asObject()->is<LocaleObject>()) {
    tagString = tagValue.asObject()->as<LocaleObject>()->locale;
  } else {
    String* s = TRY(ToString(tagValue));
    std::optional<std::string> ascii = toAscii(s->view());
    if (!ascii) return ThrowRangeError("Incorrect locale information provided");
    tagString = *ascii;
  }
  Object* options = TRY(CoerceOptionsToObject(call.args[1]));
  std::optional<LanguageTag> tag = ParseLanguageTag(std::string_view(tagString));
  if (!tag) return ThrowRangeError("Incorrect locale information provided: " + tagString);
  TRY(UpdateLanguageId(*tag, options));
  CanonicalizeUnicodeLocaleId(*tag);

  std::vector<std::pair<std::string, std::optional<std::string>>> opt;
  opt.emplace_back("ca", TRY(typeOption(options, u"calendar")));
  opt.emplace_back("co", TRY(typeOption(options, u"collation")));
  std::optional<std::string> fw = TRY(GetStringOption(options, u"firstDayOfWeek", {}));
  if (fw) {
    fw = weekdayToString(*fw);
    if (!isUnicodeLocaleType(*fw)) return ThrowRangeError("Intl.Locale: invalid firstDayOfWeek " + *fw);
  }
  opt.emplace_back("fw", fw);
  opt.emplace_back("hc", TRY(GetStringOption(options, u"hourCycle", {"h11", "h12", "h23", "h24"})));
  opt.emplace_back("kf", TRY(GetStringOption(options, u"caseFirst", {"upper", "lower", "false"})));
  std::optional<bool> kn = TRY(GetBooleanOption(options, u"numeric"));
  opt.emplace_back("kn", kn ? std::optional<std::string>(*kn ? "true" : "false") : std::nullopt);
  opt.emplace_back("nu", TRY(typeOption(options, u"numberingSystem")));

  // MakeLocaleRecord ( tag, options, localeExtensionKeys )
  for (const auto& [key, override] : opt) {
    if (!override) continue;
    std::string value = CanonicalizeUValue(key, asciiLower(*override));
    bool found = false;
    for (Keyword& k : tag->keywords) {
      if (k.key == key) {
        k.value = value;
        found = true;
      }
    }
    if (!found) tag->keywords.push_back(Keyword{key, value});
    tag->hasUnicodeExtension = true;
  }
  CanonicalizeUnicodeLocaleId(*tag);
  locale->locale = tag->toString();
  locale->calendar = keywordValue(*tag, "ca");
  locale->collation = keywordValue(*tag, "co");
  locale->firstDayOfWeek = keywordValue(*tag, "fw");
  locale->hourCycle = keywordValue(*tag, "hc");
  locale->caseFirst = keywordValue(*tag, "kf");
  std::optional<std::string> numeric = keywordValue(*tag, "kn");
  locale->numeric = numeric && (numeric->empty() || *numeric == "true");
  locale->numberingSystem = keywordValue(*tag, "nu");
  return Value(locale);
}

Result<LocaleObject*> thisLocale(Value v, const char* method) { return thisIntlObject<LocaleObject>(v, method); }

LanguageTag parsedTag(LocaleObject* loc) { return *ParseLanguageTag(std::string_view(loc->locale)); }

Result<Value> Locale_toString(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "Intl.Locale.prototype.toString"));
  return jsString(loc->locale);
}

Result<Value> constructLocale(const std::string& tag) {
  Object* ctor = builtins::intrinsic(Intrinsic::IntlLocale);
  Object* o = TRY(Construct(ctor, {jsString(tag)}));
  return Value(o);
}

Result<Value> Locale_maximize(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "Intl.Locale.prototype.maximize"));
  LanguageTag tag = parsedTag(loc);
  AddLikelySubtags(tag.id);
  return constructLocale(tag.toString());
}

Result<Value> Locale_minimize(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "Intl.Locale.prototype.minimize"));
  LanguageTag tag = parsedTag(loc);
  RemoveLikelySubtags(tag.id);
  return constructLocale(tag.toString());
}

Value optionalValue(const std::optional<std::string>& s) { return s ? jsString(*s) : Value::undefined(); }

template <std::optional<std::string> LocaleObject::*field>
Result<Value> slotGetter(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "Intl.Locale getter"));
  return optionalValue(loc->*field);
}

Result<Value> Locale_numeric(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "get Intl.Locale.prototype.numeric"));
  return boolean(loc->numeric);
}

Result<Value> Locale_baseName(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "get Intl.Locale.prototype.baseName"));
  return jsString(parsedTag(loc).id.toString());
}

Result<Value> Locale_language(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "get Intl.Locale.prototype.language"));
  return jsString(parsedTag(loc).id.language);
}

Result<Value> Locale_script(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "get Intl.Locale.prototype.script"));
  LanguageTag t = parsedTag(loc);
  return t.id.script.empty() ? Value::undefined() : jsString(t.id.script);
}

Result<Value> Locale_region(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "get Intl.Locale.prototype.region"));
  LanguageTag t = parsedTag(loc);
  return t.id.region.empty() ? Value::undefined() : jsString(t.id.region);
}

Result<Value> Locale_variants(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "get Intl.Locale.prototype.variants"));
  LanguageTag t = parsedTag(loc);
  return t.id.variants.empty() ? Value::undefined() : jsString(join(t.id.variants, "-"));
}

// ---- Locale Info ----

// The region of a subdivision or region-override value ("inka", "uszzzz", "001zzzz").
std::string regionOfSubdivision(const std::string& value) {
  if (value.size() >= 3 && value[0] >= '0' && value[0] <= '9') return value.substr(0, 3);
  if (value.size() >= 2) return asciiUpper(value.substr(0, 2));
  return {};
}

// RegionPreference ( locale ): the rg override, the region subtag, the sd subdivision's region, the likely region.
std::string regionPreference(const LanguageTag& tag) {
  if (const std::string* rg = tag.keyword("rg"); rg && rg->size() >= 3) return regionOfSubdivision(*rg);
  if (!tag.id.region.empty()) return tag.id.region;
  if (const std::string* sd = tag.keyword("sd"); sd && sd->size() >= 3) return regionOfSubdivision(*sd);
  LanguageId max = tag.id;
  AddLikelySubtags(max);
  return max.region;
}

Object* arrayOf(const std::vector<std::string>& list) {
  ValueVector values;
  for (const std::string& s : list) values.push_back(jsString(s));
  return CreateArrayFromList(values);
}

// The best available data locale for a tag's base name.
std::string dataLocaleOf(const LanguageTag& tag) {
  return BestAvailableLocale(data::locales(), tag.id.toString()).value_or(DefaultLocale());
}

Result<Value> Locale_getCalendars(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "Intl.Locale.prototype.getCalendars"));
  if (loc->calendar) return Value(arrayOf({*loc->calendar}));
  LanguageTag tag = parsedTag(loc);
  std::vector<std::string> list = data::regionCalendars(regionPreference(tag));
  std::string preferred = data::defaultCalendar(dataLocaleOf(tag));
  auto it = std::find(list.begin(), list.end(), preferred);
  if (it != list.end()) list.erase(it);
  list.insert(list.begin(), preferred);
  return Value(arrayOf(list));
}

Result<Value> Locale_getCollations(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "Intl.Locale.prototype.getCollations"));
  if (loc->collation) return Value(arrayOf({*loc->collation}));
  return Value(arrayOf(data::collations(dataLocaleOf(parsedTag(loc)))));
}

Result<Value> Locale_getHourCycles(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "Intl.Locale.prototype.getHourCycles"));
  if (loc->hourCycle) return Value(arrayOf({*loc->hourCycle}));
  return Value(arrayOf(data::regionHourCycles(regionPreference(parsedTag(loc)))));
}

Result<Value> Locale_getNumberingSystems(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "Intl.Locale.prototype.getNumberingSystems"));
  if (loc->numberingSystem) return Value(arrayOf({*loc->numberingSystem}));
  return Value(arrayOf({data::defaultNumberingSystem(dataLocaleOf(parsedTag(loc)))}));
}

Result<Value> Locale_getTimeZones(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "Intl.Locale.prototype.getTimeZones"));
  LanguageTag tag = parsedTag(loc);
  if (tag.id.region.empty()) return Value::undefined();
  return Value(arrayOf(data::regionTimeZones(tag.id.region)));
}

Result<Value> Locale_getTextInfo(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "Intl.Locale.prototype.getTextInfo"));
  LanguageTag tag = parsedTag(loc);
  LanguageId max = tag.id;
  if (max.script.empty()) AddLikelySubtags(max);
  Object* info = builtins::newPlainObject();
  addProperty(info, u"direction", str(data::isRtlScript(max.script) ? u"rtl" : u"ltr"));
  return Value(info);
}

Result<Value> Locale_getWeekInfo(NativeCall& call) {
  LocaleObject* loc = TRY(thisLocale(call.thisValue, "Intl.Locale.prototype.getWeekInfo"));
  LanguageTag tag = parsedTag(loc);
  data::WeekData w = data::regionWeek(regionPreference(tag));
  int firstDay = w.firstDay;
  if (loc->firstDayOfWeek) {
    static const char* const days[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
    for (int i = 0; i < 7; i++)
      if (*loc->firstDayOfWeek == days[i]) firstDay = i + 1;
  }
  Object* info = builtins::newPlainObject();
  addProperty(info, u"firstDay", num(firstDay));
  ValueVector weekend;
  std::vector<int> days;
  for (int d = w.weekendStart;; d = d % 7 + 1) {
    days.push_back(d);
    if (d == w.weekendEnd || days.size() == 7) break;
  }
  std::sort(days.begin(), days.end());
  for (int d : days) weekend.push_back(num(d));
  addProperty(info, u"weekend", Value(CreateArrayFromList(weekend)));
  addProperty(info, u"minimalDays", num(w.minimalDays));
  return Value(info);
}

}  // namespace

void installLocale(Realm& realm, Object* intl) {
  Object* proto = nullptr;
  defineIntlConstructor(realm, intl, Intrinsic::IntlLocale, Locale_construct, 1, u"Locale", Intrinsic::IntlLocalePrototype, &proto);
  defineFunction(realm, proto, PropertyKey(u"getCalendars"), 0, Locale_getCalendars);
  defineFunction(realm, proto, PropertyKey(u"getCollations"), 0, Locale_getCollations);
  defineFunction(realm, proto, PropertyKey(u"getHourCycles"), 0, Locale_getHourCycles);
  defineFunction(realm, proto, PropertyKey(u"getNumberingSystems"), 0, Locale_getNumberingSystems);
  defineFunction(realm, proto, PropertyKey(u"getTextInfo"), 0, Locale_getTextInfo);
  defineFunction(realm, proto, PropertyKey(u"getTimeZones"), 0, Locale_getTimeZones);
  defineFunction(realm, proto, PropertyKey(u"getWeekInfo"), 0, Locale_getWeekInfo);
  defineFunction(realm, proto, PropertyKey(u"maximize"), 0, Locale_maximize);
  defineFunction(realm, proto, PropertyKey(u"minimize"), 0, Locale_minimize);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, Locale_toString);
  defineAccessor(realm, proto, PropertyKey(u"baseName"), Locale_baseName, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"calendar"), slotGetter<&LocaleObject::calendar>, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"caseFirst"), slotGetter<&LocaleObject::caseFirst>, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"collation"), slotGetter<&LocaleObject::collation>, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"firstDayOfWeek"), slotGetter<&LocaleObject::firstDayOfWeek>, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"hourCycle"), slotGetter<&LocaleObject::hourCycle>, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"language"), Locale_language, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"numberingSystem"), slotGetter<&LocaleObject::numberingSystem>, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"numeric"), Locale_numeric, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"region"), Locale_region, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"script"), Locale_script, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"variants"), Locale_variants, nullptr);
}

}  // namespace snowman::intl
