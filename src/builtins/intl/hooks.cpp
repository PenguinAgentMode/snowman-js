// The ECMA-402 locale-sensitive methods of String, Number and BigInt (hooks.h).
#include "builtins/intl/hooks.h"

#include "builtins/intl/intl.h"
#include "builtins/intl/services.h"

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

bool isMoreAbove(char16_t c) {
  // Combining marks of class 230 (above) that follow i/j in Lithuanian: the common ones.
  return (c >= 0x0300 && c <= 0x0314) || c == 0x033D || c == 0x033E || c == 0x033F || c == 0x0340 || c == 0x0341 ||
         c == 0x0342 || c == 0x0343 || c == 0x0344 || c == 0x0346 || c == 0x034A || c == 0x034B || c == 0x034C ||
         c == 0x0350 || c == 0x0351 || c == 0x0352 || c == 0x0357 || c == 0x035B || (c >= 0x0363 && c <= 0x036F);
}

// A combining mark of class 230 or 0 that blocks After_I / After_Soft_Dotted (approximated by the combining block).
bool isCombining(char16_t c) { return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF); }

bool isSoftDotted(char16_t c) {
  return c == u'i' || c == u'j' || c == 0x012F || c == 0x0249 || c == 0x0268 || c == 0x029D || c == 0x02B2 || c == 0x03F3 ||
         c == 0x0456 || c == 0x0458 || c == 0x1D62 || c == 0x1D96 || c == 0x1DA4 || c == 0x1DA8 || c == 0x1E2D || c == 0x1ECB ||
         c == 0x2071 || c == 0x2148 || c == 0x2149 || c == 0x2C7C;
}

}  // namespace

// The conditional mappings of SpecialCasing.txt for tr, az and lt, then the default full mapping.
String* transformCase(String* s, const std::string& language, bool upper, CaseMapping defaultMapping) {
  if (language.empty()) return defaultMapping(s, upper);
  std::u16string_view v = s->view();
  std::u16string out;
  if (language == "tr" || language == "az") {
    for (size_t i = 0; i < v.size(); i++) {
      char16_t c = v[i];
      if (upper) {
        out.push_back(c == u'i' ? char16_t(0x0130) : c);
        continue;
      }
      if (c == 0x0130) {
        out.push_back(u'i');
      } else if (c == u'I') {
        // I followed by U+0307 (with no class-230 mark between) lowercases to i, dropping the dot; otherwise ı.
        size_t j = i + 1;
        while (j < v.size() && isCombining(v[j]) && v[j] != 0x0307 && !isMoreAbove(v[j])) j++;
        if (j < v.size() && v[j] == 0x0307) {
          out.push_back(u'i');
          out.append(v.substr(i + 1, j - i - 1));
          i = j;
        } else {
          out.push_back(0x0131);
        }
      } else if (c == 0x0307 && i > 0) {
        // After_I: a dot above after I (already handled) or i stays.
        out.push_back(c);
      } else {
        out.push_back(c);
      }
    }
  } else {  // lt
    for (size_t i = 0; i < v.size(); i++) {
      char16_t c = v[i];
      if (upper) {
        // After_Soft_Dotted: drop U+0307 after a soft-dotted letter (with only non-230 marks between).
        if (c == 0x0307) {
          size_t j = i;
          while (j > 0 && isCombining(v[j - 1]) && !isMoreAbove(v[j - 1])) j--;
          if (j > 0 && isSoftDotted(v[j - 1])) continue;
        }
        out.push_back(c);
        continue;
      }
      bool moreAbove = false;
      for (size_t j = i + 1; j < v.size() && isCombining(v[j]); j++)
        if (isMoreAbove(v[j])) moreAbove = true;
      switch (c) {
        case u'I': out += moreAbove ? u"i\u0307" : u"i"; break;
        case u'J': out += moreAbove ? u"j\u0307" : u"j"; break;
        case 0x012E: out += moreAbove ? u"\u012F\u0307" : u"\u012F"; break;
        case 0x00CC: out += u"i\u0307\u0300"; break;
        case 0x00CD: out += u"i\u0307\u0301"; break;
        case 0x0128: out += u"i\u0307\u0303"; break;
        default: out.push_back(c);
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
