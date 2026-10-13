// Intl.Collator (ECMA-402 10) and CompareStrings: a three-level comparison in the spirit of the UCA root
// collation (variable characters, digits, then letters; accents at level 2, case at level 3), with German
// phonebook/search expansions, numeric collation and alternate shifted (ignorePunctuation).
#include <algorithm>

#include "builtins/intl/intl.h"
#include "builtins/intl/services.h"
#include "builtins/normalize.h"
#include "builtins/string_util.h"
#include "unicode/unicode.h"

namespace snowman::intl {

namespace {

constexpr const char* kRelevantKeys[] = {"co", "kf", "kn"};

std::vector<std::string> collatorKeyValues(const std::string& locale, std::string_view key) {
  if (key == "co") {
    std::vector<std::string> out{""};  // null: the default collation
    for (const std::string& c : data::collations(locale)) out.push_back(c);
    return out;
  }
  if (key == "kf") return {"false", "lower", "upper"};
  if (key == "kn") return {"false", "true"};
  return {};
}

const ServiceData kCollatorData{data::locales, collatorKeyValues};

// ---- character classes ----

bool inSet(std::string_view property, char32_t cp) {
  std::optional<unicode::Property> p = unicode::lookupLoneProperty(property);
  if (!p) return false;
  std::span<const unicode::CodePointRange> ranges = unicode::propertyCodePoints(*p);
  auto it = std::upper_bound(ranges.begin(), ranges.end(), cp,
                             [](char32_t c, const unicode::CodePointRange& r) { return c < r.first; });
  return it != ranges.begin() && cp <= (it - 1)->last;
}

enum class Kind : uint8_t { Ignorable, Mark, Space, Punct, Symbol, Digit, Letter, Other };

Kind kindOf(char32_t cp) {
  if (cp < 0x80) {
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) return Kind::Letter;
    if (cp >= '0' && cp <= '9') return Kind::Digit;
    if (cp == ' ' || (cp >= 0x09 && cp <= 0x0d)) return Kind::Space;
    if (cp < 0x20 || cp == 0x7f) return Kind::Ignorable;
    if (std::string_view("!\"#%&'()*,-./:;?@[\\]_{}").find(char(cp)) != std::string_view::npos) return Kind::Punct;
    return Kind::Symbol;
  }
  static const char* const marks = "M";
  if (inSet(marks, cp)) return Kind::Mark;
  if (inSet("Cc", cp) || inSet("Cf", cp)) return Kind::Ignorable;
  if (inSet("Z", cp)) return Kind::Space;
  if (inSet("P", cp)) return Kind::Punct;
  if (inSet("S", cp)) return Kind::Symbol;
  if (inSet("Nd", cp)) return Kind::Digit;
  if (inSet("L", cp)) return Kind::Letter;
  return Kind::Other;
}

bool isUpper(char32_t cp) { return cp < 0x80 ? (cp >= 'A' && cp <= 'Z') : (inSet("Lu", cp) || inSet("Lt", cp)); }

int digitValue(char32_t cp) {
  if (cp >= '0' && cp <= '9') return int(cp - '0');
  // Nd digits come in runs of ten starting at a code point ending in 0 (or 6 for some): find the run's zero.
  for (char32_t zero = cp; zero + 9 >= cp && zero > 0; zero--) {
    if (!inSet("Nd", zero - 1) || cp - zero >= 9) {
      if (inSet("Nd", zero)) return int(cp - zero) % 10;
      break;
    }
  }
  return 0;
}

// A collation element: primary 0 means ignorable at level 1.
struct CE {
  uint32_t primary;
  uint16_t secondary;
  uint8_t tertiary;
  bool variable;
};

constexpr uint16_t kCommonSecondary = 5;
constexpr uint32_t kSpaceBase = 0x100, kPunctBase = 0x1000, kSymbolBase = 0x20000, kDigitBase = 0x40000,
                   kLatinBase = 0x50000, kLetterBase = 0x60000, kOtherBase = 0x200000;

struct Tailoring {
  bool germanExpansions = false;  // ä ö ü as ae oe ue (phonebook, search)
  bool numeric = false;
  bool upperFirst = false;
  bool ignorePunctuation = false;
};

uint8_t caseTertiary(bool upper, const Tailoring& t) { return (upper != t.upperFirst) ? 0x0f : kCommonSecondary; }

void appendLetter(std::vector<CE>& out, char32_t cp, const Tailoring& t) {
  bool upper = isUpper(cp);
  char32_t lower = unicode::simpleCaseFold(cp);
  auto latin = [&](char c, uint8_t tert) { out.push_back({kLatinBase + uint32_t(c - 'a') * 4, kCommonSecondary, tert, false}); };
  uint8_t tert = caseTertiary(upper, t);
  if (lower >= 'a' && lower <= 'z') return latin(char(lower), tert);
  switch (lower) {
    case 0xdf: latin('s', tert); latin('s', tert + 1); return;          // ß
    case 0xe6: latin('a', tert + 1); latin('e', tert + 1); return;      // æ
    case 0x153: latin('o', tert + 1); latin('e', tert + 1); return;     // œ
    case 0xf8: latin('o', tert); out.push_back({0, 0x40, kCommonSecondary, false}); return;   // ø
    case 0x111: latin('d', tert); out.push_back({0, 0x41, kCommonSecondary, false}); return;  // đ
    case 0x142: latin('l', tert); out.push_back({0, 0x42, kCommonSecondary, false}); return;  // ł
    case 0x131: latin('i', tert); out.push_back({0, 0x43, kCommonSecondary, false}); return;  // ı
    case 0xfe: out.push_back({kLatinBase + 26 * 4, kCommonSecondary, tert, false}); return;   // þ
    default: break;
  }
  out.push_back({kLetterBase + uint32_t(lower), kCommonSecondary, tert, false});
}

std::vector<CE> collationElements(std::u16string_view input, const Tailoring& t) {
  std::u16string nfd = builtins::Normalize(input, builtins::NormalizationForm::NFD);
  std::vector<char32_t> cps;
  for (size_t i = 0; i < nfd.size();) {
    builtins::CodePointRecord r = builtins::CodePointAt(nfd, i);
    cps.push_back(r.codePoint);
    i += size_t(r.codeUnitCount);
  }
  std::vector<CE> out;
  for (size_t i = 0; i < cps.size(); i++) {
    char32_t cp = cps[i];
    switch (kindOf(cp)) {
      case Kind::Ignorable: break;
      case Kind::Mark: out.push_back({0, uint16_t(0x20 + (cp & 0x3ff)), kCommonSecondary, false}); break;
      case Kind::Space: out.push_back({kSpaceBase + uint32_t(cp & 0xfff), kCommonSecondary, kCommonSecondary, true}); break;
      case Kind::Punct: out.push_back({kPunctBase + uint32_t(cp), kCommonSecondary, kCommonSecondary, true}); break;
      case Kind::Symbol: out.push_back({kSymbolBase + uint32_t(cp), kCommonSecondary, kCommonSecondary, false}); break;
      case Kind::Digit:
        if (t.numeric) {
          size_t j = i;
          std::vector<int> digits;
          while (j < cps.size() && kindOf(cps[j]) == Kind::Digit) digits.push_back(digitValue(cps[j++]));
          size_t k = 0;
          while (k + 1 < digits.size() && digits[k] == 0) k++;
          out.push_back({kDigitBase + 0x100 + uint32_t(digits.size() - k), kCommonSecondary, kCommonSecondary, false});
          for (; k < digits.size(); k++) out.push_back({kDigitBase + uint32_t(digits[k]), kCommonSecondary, kCommonSecondary, false});
          i = j - 1;
        } else {
          out.push_back({kDigitBase + uint32_t(digitValue(cp)), kCommonSecondary, uint8_t(cp < 0x80 ? kCommonSecondary : 6), false});
        }
        break;
      case Kind::Letter: {
        appendLetter(out, cp, t);
        char32_t lower = unicode::simpleCaseFold(cp);
        if (t.germanExpansions && (lower == 'a' || lower == 'o' || lower == 'u') && i + 1 < cps.size() && cps[i + 1] == 0x308) {
          out.push_back({kLatinBase + uint32_t('e' - 'a') * 4, kCommonSecondary, out.back().tertiary, false});
        }
        break;
      }
      case Kind::Other: out.push_back({kOtherBase + uint32_t(cp), kCommonSecondary, kCommonSecondary, false}); break;
    }
  }
  if (t.ignorePunctuation) {
    std::erase_if(out, [](const CE& ce) { return ce.variable; });
  }
  return out;
}

int compareLevel(const std::vector<CE>& a, const std::vector<CE>& b, int level) {
  auto weights = [level](const std::vector<CE>& ces) {
    std::vector<uint32_t> w;
    for (const CE& ce : ces) {
      if (level == 1 && ce.primary) w.push_back(ce.primary);
      if (level == 2 && ce.secondary) w.push_back(ce.secondary);
      if (level == 3 && ce.tertiary) w.push_back(ce.tertiary);
      if (level == 4 && ce.primary) w.push_back(ce.tertiary);  // the case level: tertiary of non-ignorables
    }
    return w;
  };
  std::vector<uint32_t> wa = weights(a), wb = weights(b);
  if (wa == wb) return 0;
  return std::lexicographical_compare(wa.begin(), wa.end(), wb.begin(), wb.end()) ? -1 : 1;
}

}  // namespace

CollatorObject::CollatorObject(Object* prototype) : Object(prototype) {}

void CollatorObject::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(boundCompare);
}

// 10.3.3.2 CompareStrings ( collator, x, y )
int CompareStrings(const CollatorObject& collator, std::u16string_view x, std::u16string_view y) {
  Tailoring t;
  std::string language = collator.locale.substr(0, collator.locale.find('-'));
  t.germanExpansions = language == "de" && (collator.collation == "phonebk" || collator.usage == "search");
  t.numeric = collator.numeric;
  t.upperFirst = collator.caseFirst == "upper";
  t.ignorePunctuation = collator.ignorePunctuation;
  std::vector<CE> a = collationElements(x, t), b = collationElements(y, t);
  if (int r = compareLevel(a, b, 1)) return r;
  const std::string& s = collator.sensitivity;
  if (s == "accent" || s == "variant") {
    if (int r = compareLevel(a, b, 2)) return r;
  }
  if (s == "case") {
    if (int r = compareLevel(a, b, 4)) return r;
  }
  if (s == "variant") {
    if (int r = compareLevel(a, b, 3)) return r;
  }
  return 0;
}

namespace {

// 10.1.1 Intl.Collator ( [ locales [ , options ] ] )
Result<Value> Collator_construct(NativeCall& call) {
  Object* newTarget = builtins::newTargetOrCallee(call);
  CollatorObject* collator = TRY(OrdinaryCreateFromConstructor<CollatorObject>(newTarget, Intrinsic::IntlCollatorPrototype));
  TRY(InitializeCollator(collator, call.args[0], call.args[1]));
  return Value(collator);
}

Result<Value> Collator_supportedLocalesOf(NativeCall& call) { return supportedLocalesOf(kCollatorData, call); }

Result<Value> boundCompare(NativeCall& call) {
  CollatorObject* collator = call.callee->slots[0].asObject()->as<CollatorObject>();
  String* x = TRY(ToString(call.args[0]));
  String* y = TRY(ToString(call.args[1]));
  return num(CompareStrings(*collator, x->view(), y->view()));
}

// 10.3.3 get Intl.Collator.prototype.compare
Result<Value> CollatorPrototype_compare(NativeCall& call) {
  CollatorObject* collator = TRY(thisIntlObject<CollatorObject>(call.thisValue, "get Intl.Collator.prototype.compare"));
  if (!collator->boundCompare) {
    BuiltinFunction* f = createBuiltinFunction(*builtins::currentRealm(), boundCompare, 2, PropertyKey(u""));
    f->slots.push_back(Value(collator));
    collator->boundCompare = f;
  }
  return Value(collator->boundCompare);
}

// 10.3.4 Intl.Collator.prototype.resolvedOptions ( )
Result<Value> CollatorPrototype_resolvedOptions(NativeCall& call) {
  CollatorObject* c = TRY(thisIntlObject<CollatorObject>(call.thisValue, "Intl.Collator.prototype.resolvedOptions"));
  Object* o = builtins::newPlainObject();
  addProperty(o, u"locale", jsString(c->locale));
  addProperty(o, u"usage", jsString(c->usage));
  addProperty(o, u"sensitivity", jsString(c->sensitivity));
  addProperty(o, u"ignorePunctuation", boolean(c->ignorePunctuation));
  addProperty(o, u"collation", jsString(c->collation));
  addProperty(o, u"numeric", boolean(c->numeric));
  addProperty(o, u"caseFirst", jsString(c->caseFirst));
  return Value(o);
}

}  // namespace

// 10.1.2 InitializeCollator ( collator, locales, options )
Result<void> InitializeCollator(CollatorObject* collator, Value locales, Value optionsValue) {
  std::vector<std::string> requested = TRY(CanonicalizeLocaleList(locales));
  Object* options = TRY(CoerceOptionsToObject(optionsValue));
  collator->usage = *TRY(GetStringOption(options, u"usage", {"sort", "search"}, "sort"));
  LocaleOptions opt;
  opt.localeMatcher = TRY(GetLocaleMatcher(options));
  std::optional<std::string> collation = TRY(GetStringOption(options, u"collation", {}));
  if (collation && !isUnicodeLocaleType(*collation)) return ThrowRangeError("Intl.Collator: invalid collation " + *collation);
  if (collation) opt.keys.emplace_back("co", LocaleOption::string(*collation));
  std::optional<bool> numeric = TRY(GetBooleanOption(options, u"numeric"));
  if (numeric) opt.keys.emplace_back("kn", LocaleOption::string(*numeric ? "true" : "false"));
  std::optional<std::string> caseFirst = TRY(GetStringOption(options, u"caseFirst", {"upper", "lower", "false"}));
  if (caseFirst) opt.keys.emplace_back("kf", LocaleOption::string(*caseFirst));
  ResolvedLocale r = ResolveLocale(kCollatorData, requested, opt, kRelevantKeys);
  collator->locale = r.locale;
  collator->collation = r.get("co").empty() ? "default" : r.get("co");
  collator->numeric = r.get("kn") == "true";
  collator->caseFirst = r.get("kf");
  std::optional<std::string> sensitivity = TRY(GetStringOption(options, u"sensitivity", {"base", "accent", "case", "variant"}));
  collator->sensitivity = sensitivity ? *sensitivity : "variant";
  std::optional<bool> ignorePunctuation = TRY(GetBooleanOption(options, u"ignorePunctuation"));
  std::string language = r.dataLocale.substr(0, r.dataLocale.find('-'));
  collator->ignorePunctuation = ignorePunctuation ? *ignorePunctuation : language == "th";
  return {};
}

void installCollator(Realm& realm, Object* intl) {
  Object* proto = nullptr;
  BuiltinFunction* ctor = defineIntlConstructor(realm, intl, Intrinsic::IntlCollator, Collator_construct, 0, u"Collator",
                                                Intrinsic::IntlCollatorPrototype, &proto);
  defineFunction(realm, ctor, PropertyKey(u"supportedLocalesOf"), 1, Collator_supportedLocalesOf);
  defineAccessor(realm, proto, PropertyKey(u"compare"), CollatorPrototype_compare, nullptr);
  defineFunction(realm, proto, PropertyKey(u"resolvedOptions"), 0, CollatorPrototype_resolvedOptions);
}

}  // namespace snowman::intl
