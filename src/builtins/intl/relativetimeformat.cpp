// Intl.RelativeTimeFormat (ECMA-402 18): patterns by plural category and the numeric "auto" names from
// relative_data.inc, the number through Intl.NumberFormat.
#include <algorithm>
#include <cmath>

#include "builtins/intl/intl.h"
#include "builtins/intl/services.h"

namespace snowman::intl {

namespace {

struct DataEntry {
  const char* key;
  const char16_t* value;
};
#include "builtins/intl/relative_data.inc"

class RelativeTimeFormatObject : public Object {
 public:
  using Object::Object;
  std::string locale, dataLocale, numberingSystem, style, numeric;
  NumberFormatObject* numberFormat = nullptr;  // [[NumberFormat]]
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(numberFormat);
  }
};

constexpr const char* kRelevantKeys[] = {"nu"};

std::vector<std::string> rtfKeyValues(const std::string& locale, std::string_view key) {
  if (key != "nu") return {};
  std::vector<std::string> out{data::defaultNumberingSystem(locale)};
  for (const std::string& nu : data::numberingSystems())
    if (nu != out[0]) out.push_back(nu);
  return out;
}
const ServiceData kRelativeTimeFormatData{data::locales, rtfKeyValues};

std::u16string_view patternData(const std::string& locale, const std::string& unit, const std::string& style) {
  std::string suffix = "/" + unit + "/" + style;
  auto find = [](const std::string& key) -> const char16_t* {
    const DataEntry* end = std::end(kRelativePatterns);
    const DataEntry* it = std::lower_bound(std::begin(kRelativePatterns), end, key,
                                           [](const DataEntry& e, const std::string& k) { return std::string_view(e.key) < k; });
    return it != end && key == it->key ? it->value : nullptr;
  };
  for (const std::string& c : data::lookupChain(locale))
    if (const char16_t* v = find(c + suffix)) return v;
  return find(suffix);
}

std::vector<std::u16string_view> splitOn(std::u16string_view s, char16_t sep) {
  std::vector<std::u16string_view> out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(sep, start);
    out.push_back(s.substr(start, pos == std::u16string_view::npos ? std::u16string_view::npos : pos - start));
    if (pos == std::u16string_view::npos) return out;
    start = pos + 1;
  }
}

// 18.5.1 SingularRelativeTimeUnit ( unit )
Result<std::string> singularUnit(String* unit) {
  static const char* const units[] = {"second", "minute", "hour", "day", "week", "month", "quarter", "year"};
  std::u16string_view u = unit->view();
  for (const char* name : units) {
    std::u16string singular = builtins::toU16(name);
    if (u == singular || u == singular + u"s") return std::string(name);
  }
  return ThrowRangeError("Intl.RelativeTimeFormat: invalid unit " + unit->toUtf8());
}

struct RelativePart {
  const char16_t* type;
  std::u16string value;
  bool hasUnit;
};

// 18.5.2 PartitionRelativeTimePattern ( relativeTimeFormat, value, unit )
Result<std::pair<std::vector<RelativePart>, std::string>> partition(const RelativeTimeFormatObject& rtf, Value valueArg, Value unitArg) {
  double value = TRY(ToNumber(valueArg));
  String* unitString = TRY(ToString(unitArg));
  if (!std::isfinite(value)) return ThrowRangeError("Intl.RelativeTimeFormat: value must be finite");
  std::string unit = TRY(singularUnit(unitString));
  std::vector<std::u16string_view> fields = splitOn(patternData(rtf.dataLocale, unit, rtf.style), u'\x1f');
  fields.resize(3);
  std::vector<RelativePart> parts;
  if (rtf.numeric == "auto") {
    std::u16string valueString(NumberToString(value)->view());
    for (std::u16string_view entry : splitOn(fields[2], u'\x1d')) {
      size_t eq = entry.find(u'=');
      if (eq != std::u16string_view::npos && entry.substr(0, eq) == valueString) {
        parts.push_back({u"literal", std::u16string(entry.substr(eq + 1)), false});
        return std::make_pair(parts, unit);
      }
    }
  }
  bool past = value < 0 || (value == 0 && std::signbit(value));
  Decimal d = Decimal::fromDouble(std::fabs(value));
  char category = numberFormatPluralCategory(*rtf.numberFormat, d);
  std::u16string_view chosen, other;
  for (std::u16string_view p : splitOn(fields[past ? 1 : 0], u'\x1d')) {
    if (p.empty()) continue;
    if (p[0] == char16_t(category)) chosen = p.substr(1);
    if (p[0] == u'x') other = p.substr(1);
  }
  if (chosen.empty()) chosen = other;
  if (!chosen.empty() && chosen[0] == u'\x02') {
    parts.push_back({u"literal", std::u16string(chosen.substr(1)), false});
    return std::make_pair(parts, unit);
  }
  size_t sep = chosen.find(u'\x1e');
  std::u16string_view before = chosen.substr(0, sep), after = sep == std::u16string_view::npos ? std::u16string_view() : chosen.substr(sep + 1);
  if (!before.empty()) parts.push_back({u"literal", std::u16string(before), false});
  for (NumberPart& p : PartitionNumberPattern(*rtf.numberFormat, d)) parts.push_back({p.type, std::move(p.value), true});
  if (!after.empty()) parts.push_back({u"literal", std::u16string(after), false});
  return std::make_pair(parts, unit);
}

// 18.1.1 Intl.RelativeTimeFormat ( [ locales [ , options ] ] )
Result<Value> RelativeTimeFormat_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Intl.RelativeTimeFormat requires 'new'");
  RelativeTimeFormatObject* rtf =
      TRY(OrdinaryCreateFromConstructor<RelativeTimeFormatObject>(call.newTarget, Intrinsic::IntlRelativeTimeFormatPrototype));
  std::vector<std::string> requested = TRY(CanonicalizeLocaleList(call.args[0]));
  Object* options = TRY(CoerceOptionsToObject(call.args[1]));
  LocaleOptions opt;
  opt.localeMatcher = TRY(GetLocaleMatcher(options));
  std::optional<std::string> numberingSystem = TRY(GetStringOption(options, u"numberingSystem", {}));
  if (numberingSystem && !isUnicodeLocaleType(*numberingSystem))
    return ThrowRangeError("Intl.RelativeTimeFormat: invalid numberingSystem " + *numberingSystem);
  if (numberingSystem) opt.keys.emplace_back("nu", LocaleOption::string(*numberingSystem));
  ResolvedLocale r = ResolveLocale(kRelativeTimeFormatData, requested, opt, kRelevantKeys);
  rtf->locale = r.locale;
  rtf->dataLocale = r.dataLocale;
  rtf->numberingSystem = r.get("nu");
  rtf->style = *TRY(GetStringOption(options, u"style", {"long", "short", "narrow"}, "long"));
  rtf->numeric = *TRY(GetStringOption(options, u"numeric", {"always", "auto"}, "always"));
  NumberFormatObject* nf = TRY(createNumberFormat(jsString(rtf->locale), Value::undefined()));
  nf->numberingSystem = rtf->numberingSystem;
  rtf->numberFormat = nf;
  return Value(rtf);
}

Result<Value> RelativeTimeFormat_supportedLocalesOf(NativeCall& call) { return supportedLocalesOf(kRelativeTimeFormatData, call); }

Result<Value> RelativeTimeFormatPrototype_format(NativeCall& call) {
  RelativeTimeFormatObject* rtf = TRY(thisIntlObject<RelativeTimeFormatObject>(call.thisValue, "Intl.RelativeTimeFormat.prototype.format"));
  auto [parts, unit] = TRY(partition(*rtf, call.args[0], call.args[1]));
  std::u16string out;
  for (const RelativePart& p : parts) out += p.value;
  return Value(newString(std::move(out)));
}

Result<Value> RelativeTimeFormatPrototype_formatToParts(NativeCall& call) {
  RelativeTimeFormatObject* rtf =
      TRY(thisIntlObject<RelativeTimeFormatObject>(call.thisValue, "Intl.RelativeTimeFormat.prototype.formatToParts"));
  auto [parts, unit] = TRY(partition(*rtf, call.args[0], call.args[1]));
  ValueVector values;
  for (const RelativePart& p : parts) {
    Object* o = builtins::newPlainObject();
    addProperty(o, u"type", str(p.type));
    addProperty(o, u"value", Value(newString(p.value)));
    if (p.hasUnit) addProperty(o, u"unit", jsString(unit));
    values.push_back(Value(o));
  }
  return Value(CreateArrayFromList(values));
}

Result<Value> RelativeTimeFormatPrototype_resolvedOptions(NativeCall& call) {
  RelativeTimeFormatObject* rtf =
      TRY(thisIntlObject<RelativeTimeFormatObject>(call.thisValue, "Intl.RelativeTimeFormat.prototype.resolvedOptions"));
  Object* o = builtins::newPlainObject();
  addProperty(o, u"locale", jsString(rtf->locale));
  addProperty(o, u"style", jsString(rtf->style));
  addProperty(o, u"numeric", jsString(rtf->numeric));
  addProperty(o, u"numberingSystem", jsString(rtf->numberingSystem));
  return Value(o);
}

}  // namespace

void installRelativeTimeFormat(Realm& realm, Object* intl) {
  Object* proto = nullptr;
  BuiltinFunction* ctor = defineIntlConstructor(realm, intl, Intrinsic::IntlRelativeTimeFormat, RelativeTimeFormat_construct, 0,
                                                u"RelativeTimeFormat", Intrinsic::IntlRelativeTimeFormatPrototype, &proto);
  defineFunction(realm, ctor, PropertyKey(u"supportedLocalesOf"), 1, RelativeTimeFormat_supportedLocalesOf);
  defineFunction(realm, proto, PropertyKey(u"format"), 2, RelativeTimeFormatPrototype_format);
  defineFunction(realm, proto, PropertyKey(u"formatToParts"), 2, RelativeTimeFormatPrototype_formatToParts);
  defineFunction(realm, proto, PropertyKey(u"resolvedOptions"), 0, RelativeTimeFormatPrototype_resolvedOptions);
}

}  // namespace snowman::intl
