// Intl.ListFormat (ECMA-402 14): patterns from list_data.inc, with the Spanish y/e, o/u and Hebrew ו- forms.
#include <algorithm>

#include "builtins/intl/intl.h"
#include "builtins/intl/services.h"

namespace snowman::intl {

namespace {

#include "builtins/intl/list_data.inc"

std::vector<std::string> noKeys(const std::string&, std::string_view) { return {}; }
const ServiceData kListFormatData{data::locales, noKeys};

std::u16string_view listPattern(const ListFormatObject& lf) {
  std::string suffix = "/" + lf.type + "/" + lf.style;
  auto find = [](const std::string& key) -> const char16_t* {
    const ListEntry* end = std::end(kListPatterns);
    const ListEntry* it = std::lower_bound(std::begin(kListPatterns), end, key,
                                           [](const ListEntry& e, const std::string& k) { return std::string_view(e.key) < k; });
    return it != end && key == it->key ? kListValues[it->value] : nullptr;
  };
  for (const std::string& c : data::lookupChain(lf.locale))
    if (const char16_t* v = find(c + suffix)) return v;
  return find(suffix);
}

std::vector<std::u16string> fieldsOf(std::u16string_view s) {
  std::vector<std::u16string> out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(u'\x1f', start);
    out.emplace_back(s.substr(start, pos == std::u16string_view::npos ? std::u16string_view::npos : pos - start));
    if (pos == std::u16string_view::npos) break;
    start = pos + 1;
  }
  out.resize(8);
  return out;
}

bool startsWithCI(std::u16string_view s, std::u16string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); i++) {
    char16_t c = s[i];
    if (c >= u'A' && c <= u'Z') c = char16_t(c + 32);
    if (c != prefix[i]) return false;
  }
  return true;
}

// The separator before `next`, with the contextual forms of Spanish and Hebrew.
std::u16string contextual(const std::string& language, std::u16string separator, std::u16string_view next) {
  if (language == "es") {
    if (separator == u" y " && (startsWithCI(next, u"i") || (startsWithCI(next, u"hi") && !(next.size() > 2 && (startsWithCI(next.substr(2), u"a") || startsWithCI(next.substr(2), u"e"))))))
      return u" e ";
    if (separator == u" o " && (startsWithCI(next, u"o") || startsWithCI(next, u"ho") || startsWithCI(next, u"8") || next == u"11"))
      return u" u ";
  }
  if (language == "he" && !separator.empty() && separator.back() == u'\u05D5' && (next.empty() || next[0] < 0x05D0 || next[0] > 0x05EA))
    return separator + u"-";
  return separator;
}

// 14.5.3 StringListFromIterable ( iterable )
Result<std::vector<std::u16string>> StringListFromIterable(Value iterable) {
  std::vector<std::u16string> list;
  if (iterable.isUndefined()) return list;
  IteratorRecord record = TRY(GetIterator(iterable, IteratorKind::Sync));
  while (true) {
    std::optional<Value> next = TRY(IteratorStepValue(record));
    if (!next) return list;
    if (!next->isString()) {
      Throw error = ThrowTypeError("Intl.ListFormat: list element is not a string");
      Result<void> closed = IteratorClose(record, error);
      return Throw{closed.thrown()};
    }
    list.emplace_back(next->asString()->view());
  }
}

// 14.1.1 Intl.ListFormat ( [ locales [ , options ] ] )
Result<Value> ListFormat_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Intl.ListFormat requires 'new'");
  ListFormatObject* lf = TRY(OrdinaryCreateFromConstructor<ListFormatObject>(call.newTarget, Intrinsic::IntlListFormatPrototype));
  std::vector<std::string> requested = TRY(CanonicalizeLocaleList(call.args[0]));
  Object* options = TRY(GetOptionsObject(call.args[1]));
  LocaleOptions opt;
  opt.localeMatcher = TRY(GetLocaleMatcher(options));
  ResolvedLocale r = ResolveLocale(kListFormatData, requested, opt, {});
  lf->locale = r.locale;
  lf->type = *TRY(GetStringOption(options, u"type", {"conjunction", "disjunction", "unit"}, "conjunction"));
  lf->style = *TRY(GetStringOption(options, u"style", {"long", "short", "narrow"}, "long"));
  return Value(lf);
}

Result<Value> ListFormat_supportedLocalesOf(NativeCall& call) { return supportedLocalesOf(kListFormatData, call); }

Result<Value> ListFormatPrototype_format(NativeCall& call) {
  ListFormatObject* lf = TRY(thisIntlObject<ListFormatObject>(call.thisValue, "Intl.ListFormat.prototype.format"));
  std::vector<std::u16string> list = TRY(StringListFromIterable(call.args[0]));
  std::u16string out;
  for (const ListPart& p : CreatePartsFromList(*lf, list)) out += p.value;
  return Value(newString(std::move(out)));
}

Result<Value> ListFormatPrototype_formatToParts(NativeCall& call) {
  ListFormatObject* lf = TRY(thisIntlObject<ListFormatObject>(call.thisValue, "Intl.ListFormat.prototype.formatToParts"));
  std::vector<std::u16string> list = TRY(StringListFromIterable(call.args[0]));
  ValueVector parts;
  for (const ListPart& p : CreatePartsFromList(*lf, list)) {
    Object* o = builtins::newPlainObject();
    addProperty(o, u"type", str(p.element ? u"element" : u"literal"));
    addProperty(o, u"value", Value(newString(p.value)));
    parts.push_back(Value(o));
  }
  return Value(CreateArrayFromList(parts));
}

Result<Value> ListFormatPrototype_resolvedOptions(NativeCall& call) {
  ListFormatObject* lf = TRY(thisIntlObject<ListFormatObject>(call.thisValue, "Intl.ListFormat.prototype.resolvedOptions"));
  Object* o = builtins::newPlainObject();
  addProperty(o, u"locale", jsString(lf->locale));
  addProperty(o, u"type", jsString(lf->type));
  addProperty(o, u"style", jsString(lf->style));
  return Value(o);
}

}  // namespace

std::vector<ListPart> CreatePartsFromList(const ListFormatObject& lf, const std::vector<std::u16string>& list) {
  std::vector<ListPart> out;
  if (list.empty()) return out;
  if (list.size() == 1) {
    out.push_back({true, list[0]});
    return out;
  }
  std::vector<std::u16string> f = fieldsOf(listPattern(lf));
  std::string language = lf.locale.substr(0, lf.locale.find('-'));
  auto literal = [&](const std::u16string& s) {
    if (!s.empty()) out.push_back({false, s});
  };
  if (list.size() == 2) {
    literal(f[0]);
    out.push_back({true, list[0]});
    literal(contextual(language, f[1], list[1]));
    out.push_back({true, list[1]});
    literal(f[2]);
    return out;
  }
  literal(f[3]);
  for (size_t i = 0; i < list.size(); i++) {
    if (i > 0) {
      const std::u16string& sep = i == 1 ? f[4] : i == list.size() - 1 ? f[6] : f[5];
      literal(contextual(language, sep, list[i]));
    }
    out.push_back({true, list[i]});
  }
  literal(f[7]);
  return out;
}

void installListFormat(Realm& realm, Object* intl) {
  Object* proto = nullptr;
  BuiltinFunction* ctor = defineIntlConstructor(realm, intl, Intrinsic::IntlListFormat, ListFormat_construct, 0, u"ListFormat",
                                                Intrinsic::IntlListFormatPrototype, &proto);
  defineFunction(realm, ctor, PropertyKey(u"supportedLocalesOf"), 1, ListFormat_supportedLocalesOf);
  defineFunction(realm, proto, PropertyKey(u"format"), 1, ListFormatPrototype_format);
  defineFunction(realm, proto, PropertyKey(u"formatToParts"), 1, ListFormatPrototype_formatToParts);
  defineFunction(realm, proto, PropertyKey(u"resolvedOptions"), 0, ListFormatPrototype_resolvedOptions);
}

}  // namespace snowman::intl
