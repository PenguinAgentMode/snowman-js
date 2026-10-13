// The ECMA-402 locale-sensitive methods of String, Number and BigInt (hooks.h).
#include "builtins/intl/hooks.h"

#include <algorithm>

#include "builtins/intl/intl.h"
#include "builtins/intl/services.h"
#include "builtins/string_util.h"

namespace snowman::intl {

Result<Value> localeCompare(String* s, String* that, Value locales, Value options) {
  CollatorObject* collator = make<CollatorObject>(builtins::intrinsic(Intrinsic::IntlCollatorPrototype));
  TRY(InitializeCollator(collator, locales, options));
  return num(CompareStrings(*collator, s->view(), that->view()));
}

Result<std::string> transformCaseLanguage(Value locales) {
  std::vector<std::string> requested = TRY(CanonicalizeLocaleList(locales));
  std::string requestedLocale = requested.empty() ? DefaultLocale() : requested[0];
  std::string noExtensions = removeUnicodeExtensions(requestedLocale);
  static const std::string available[] = {"az", "lt", "tr"};
  if (std::optional<std::string> l = BestAvailableLocale(available, noExtensions)) return *l;
  return std::string();
}

namespace {

struct CodeRange {
  char32_t first, last;
};
#include "builtins/intl/casing_data.inc"

template <size_t N>
bool inRanges(const CodeRange (&table)[N], char32_t cp) {
  const CodeRange* end = table + N;
  const CodeRange* it = std::upper_bound(table, end, cp, [](char32_t c, const CodeRange& r) { return c < r.first; });
  return it != table && cp <= (it - 1)->last;
}
bool isAbove(char32_t cp) { return inRanges(kCombiningAbove, cp); }
bool isStarter(char32_t cp) { return !inRanges(kCombiningNonzero, cp); }
// A character that ends the After_I / After_Soft_Dotted / Before_Dot context: combining class 0 or 230.
bool blocks(char32_t cp) { return isStarter(cp) || isAbove(cp); }

}  // namespace

// The conditional mappings of SpecialCasing.txt for tr, az and lt, then the default full mapping.
String* transformCase(String* s, const std::string& language, bool upper, CaseMapping defaultMapping) {
  if (language.empty()) return defaultMapping(s, upper);
  std::u16string_view v = s->view();
  std::vector<char32_t> cps;
  for (size_t i = 0; i < v.size();) {
    builtins::CodePointRecord r = builtins::CodePointAt(v, i);
    cps.push_back(r.codePoint);
    i += size_t(r.codeUnitCount);
  }
  std::u16string out;
  auto append = [&](char32_t cp) {
    if (cp >= 0x10000) {
      out.push_back(char16_t(0xD800 + ((cp - 0x10000) >> 10)));
      out.push_back(char16_t(0xDC00 + ((cp - 0x10000) & 0x3FF)));
    } else {
      out.push_back(char16_t(cp));
    }
  };
  size_t n = cps.size();
  // Before_Dot: a U+0307 follows, with only marks of classes other than 0 and 230 between.
  auto beforeDot = [&](size_t i) {
    for (size_t j = i + 1; j < n; j++) {
      if (cps[j] == 0x307) return true;
      if (blocks(cps[j])) return false;
    }
    return false;
  };
  // After the character `what` (I, or any Soft_Dotted), with only marks of classes other than 0 and 230 between.
  auto after = [&](size_t i, bool softDotted) {
    for (size_t k = i; k-- > 0;) {
      if (softDotted ? inRanges(kSoftDotted, cps[k]) : cps[k] == u'I') return true;
      if (blocks(cps[k])) return false;
    }
    return false;
  };
  // More_Above: a class-230 mark follows before the next starter.
  auto moreAbove = [&](size_t i) {
    for (size_t j = i + 1; j < n && !isStarter(cps[j]); j++)
      if (isAbove(cps[j])) return true;
    return false;
  };
  for (size_t i = 0; i < n; i++) {
    char32_t c = cps[i];
    if (language == "tr" || language == "az") {
      if (upper) {
        append(c == u'i' ? char32_t(0x0130) : c);
      } else if (c == 0x0130) {
        append(u'i');
      } else if (c == u'I') {
        append(beforeDot(i) ? char32_t(u'i') : char32_t(0x0131));
      } else if (c == 0x0307 && after(i, false)) {
        // After_I: the dot is absorbed into the i.
      } else {
        append(c);
      }
    } else {  // lt
      if (upper) {
        if (c == 0x0307 && after(i, true)) continue;  // After_Soft_Dotted
        append(c);
        continue;
      }
      switch (c) {
        case u'I': append(u'i'); if (moreAbove(i)) append(0x0307); break;
        case u'J': append(u'j'); if (moreAbove(i)) append(0x0307); break;
        case 0x012E: append(0x012F); if (moreAbove(i)) append(0x0307); break;
        case 0x00CC: append(u'i'); append(0x0307); append(0x0300); break;
        case 0x00CD: append(u'i'); append(0x0307); append(0x0301); break;
        case 0x0128: append(u'i'); append(0x0307); append(0x0303); break;
        default: append(c);
      }
    }
  }
  return defaultMapping(newString(std::move(out)), upper);
}

Result<Value> numberToLocaleString(Value x, Value locales, Value options) {
  NumberFormatObject* nf = TRY(createNumberFormat(locales, options));
  Decimal d = x.isBigInt() ? Decimal::fromBigInt(x.asBigInt()) : Decimal::fromDouble(x.asNumber());
  return Value(newString(FormatNumeric(*nf, d)));
}

}  // namespace snowman::intl
