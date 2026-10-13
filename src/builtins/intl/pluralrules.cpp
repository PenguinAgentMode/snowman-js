// Intl.PluralRules (ECMA-402 17): select and selectRange over the CLDR plural rules (number_core.cpp), with the
// number rounded as NumberFormat's digit options round it (a hidden NumberFormat carries them).
#include <cmath>

#include "builtins/intl/intl.h"
#include "builtins/intl/services.h"

namespace snowman::intl {

namespace {

class PluralRulesObject : public Object {
 public:
  using Object::Object;
  std::string locale;          // [[Locale]]
  std::string type;            // [[Type]]
  NumberFormatObject* numbers = nullptr;  // [[Notation]], [[CompactDisplay]] and the digit options
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(numbers);
  }
};

std::vector<std::string> noKeys(const std::string&, std::string_view) { return {}; }
const ServiceData kPluralRulesData{data::locales, noKeys};

// 17.1.1 Intl.PluralRules ( [ locales [ , options ] ] )
Result<Value> PluralRules_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Intl.PluralRules requires 'new'");
  PluralRulesObject* pr = TRY(OrdinaryCreateFromConstructor<PluralRulesObject>(call.newTarget, Intrinsic::IntlPluralRulesPrototype));
  std::vector<std::string> requested = TRY(CanonicalizeLocaleList(call.args[0]));
  Object* options = TRY(CoerceOptionsToObject(call.args[1]));
  LocaleOptions opt;
  opt.localeMatcher = TRY(GetLocaleMatcher(options));
  pr->type = *TRY(GetStringOption(options, u"type", {"cardinal", "ordinal"}, "cardinal"));
  NumberFormatObject* nf = make<NumberFormatObject>(nullptr);
  pr->numbers = nf;
  nf->notation = *TRY(GetStringOption(options, u"notation", {"standard", "compact", "scientific", "engineering"}, "standard"));
  std::string compactDisplay = *TRY(GetStringOption(options, u"compactDisplay", {"short", "long"}, "short"));
  if (nf->notation == "compact") nf->compactDisplay = compactDisplay;
  TRY(SetNumberFormatDigitOptions(nf->digits, options, 0, 3, nf->notation));
  ResolvedLocale r = ResolveLocale(kPluralRulesData, requested, opt, {});
  pr->locale = r.locale;
  nf->locale = r.locale;
  nf->dataLocale = r.dataLocale;
  nf->numberingSystem = "latn";
  nf->useGrouping = "false";
  return Value(pr);
}

Result<Value> PluralRules_supportedLocalesOf(NativeCall& call) { return supportedLocalesOf(kPluralRulesData, call); }

// 17.5.3 ResolvePlural ( pluralRules, n )
char resolvePlural(const PluralRulesObject& pr, double n) {
  if (!std::isfinite(n)) return 'x';
  return numberFormatPluralCategory(*pr.numbers, Decimal::fromDouble(n), pr.type == "ordinal");
}

// 17.3.3 Intl.PluralRules.prototype.select ( value )
Result<Value> PluralRulesPrototype_select(NativeCall& call) {
  PluralRulesObject* pr = TRY(thisIntlObject<PluralRulesObject>(call.thisValue, "Intl.PluralRules.prototype.select"));
  double n = TRY(ToNumber(call.args[0]));
  return jsString(categoryName(resolvePlural(*pr, n)));
}

// 17.3.4 Intl.PluralRules.prototype.selectRange ( start, end )
Result<Value> PluralRulesPrototype_selectRange(NativeCall& call) {
  PluralRulesObject* pr = TRY(thisIntlObject<PluralRulesObject>(call.thisValue, "Intl.PluralRules.prototype.selectRange"));
  if (call.args[0].isUndefined() || call.args[1].isUndefined()) return ThrowTypeError("Intl.PluralRules: selectRange needs start and end");
  double x = TRY(ToNumber(call.args[0]));
  double y = TRY(ToNumber(call.args[1]));
  if (std::isnan(x) || std::isnan(y)) return ThrowRangeError("Intl.PluralRules: selectRange with NaN");
  char xp = resolvePlural(*pr, x), yp = resolvePlural(*pr, y);
  if (std::isfinite(x) && std::isfinite(y) &&
      FormatNumeric(*pr->numbers, Decimal::fromDouble(x)) == FormatNumeric(*pr->numbers, Decimal::fromDouble(y)))
    return jsString(categoryName(xp));
  if (pr->type == "ordinal") return jsString(categoryName(yp));
  return jsString(categoryName(pluralRangeCategory(pluralRulesFor(pr->numbers->dataLocale, false), xp, yp)));
}

// 17.3.5 Intl.PluralRules.prototype.resolvedOptions ( )
Result<Value> PluralRulesPrototype_resolvedOptions(NativeCall& call) {
  PluralRulesObject* pr = TRY(thisIntlObject<PluralRulesObject>(call.thisValue, "Intl.PluralRules.prototype.resolvedOptions"));
  const NumberFormatObject& nf = *pr->numbers;
  const DigitOptions& d = nf.digits;
  Object* o = builtins::newPlainObject();
  addProperty(o, u"locale", jsString(pr->locale));
  addProperty(o, u"type", jsString(pr->type));
  addProperty(o, u"notation", jsString(nf.notation));
  if (nf.notation == "compact") addProperty(o, u"compactDisplay", jsString(nf.compactDisplay));
  addProperty(o, u"minimumIntegerDigits", num(d.minimumIntegerDigits));
  if (d.hasFractionDigits) {
    addProperty(o, u"minimumFractionDigits", num(d.minimumFractionDigits));
    addProperty(o, u"maximumFractionDigits", num(d.maximumFractionDigits));
  }
  if (d.hasSignificantDigits) {
    addProperty(o, u"minimumSignificantDigits", num(d.minimumSignificantDigits));
    addProperty(o, u"maximumSignificantDigits", num(d.maximumSignificantDigits));
  }
  ValueVector categories;
  for (char c : pluralCategories(pluralRulesFor(nf.dataLocale, pr->type == "ordinal"))) categories.push_back(jsString(categoryName(c)));
  addProperty(o, u"pluralCategories", Value(CreateArrayFromList(categories)));
  addProperty(o, u"roundingIncrement", num(d.roundingIncrement));
  addProperty(o, u"roundingMode", jsString(d.roundingMode));
  addProperty(o, u"roundingPriority", jsString(d.computedRoundingPriority));
  addProperty(o, u"trailingZeroDisplay", jsString(d.trailingZeroDisplay));
  return Value(o);
}

}  // namespace

void installPluralRules(Realm& realm, Object* intl) {
  Object* proto = nullptr;
  BuiltinFunction* ctor = defineIntlConstructor(realm, intl, Intrinsic::IntlPluralRules, PluralRules_construct, 0, u"PluralRules",
                                                Intrinsic::IntlPluralRulesPrototype, &proto);
  defineFunction(realm, ctor, PropertyKey(u"supportedLocalesOf"), 1, PluralRules_supportedLocalesOf);
  defineFunction(realm, proto, PropertyKey(u"resolvedOptions"), 0, PluralRulesPrototype_resolvedOptions);
  defineFunction(realm, proto, PropertyKey(u"select"), 1, PluralRulesPrototype_select);
  defineFunction(realm, proto, PropertyKey(u"selectRange"), 2, PluralRulesPrototype_selectRange);
}

}  // namespace snowman::intl
