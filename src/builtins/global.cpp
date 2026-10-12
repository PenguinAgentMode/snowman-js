// The global object's value properties and function properties (19.1, 19.2) and Annex B escape/unescape.
#include <cmath>
#include <cstdlib>

#include "builtins/util.h"
#include "runtime/bigint.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

int digitValue(char16_t c) {
  if (c >= u'0' && c <= u'9') return c - u'0';
  if (c >= u'a' && c <= u'z') return c - u'a' + 10;
  if (c >= u'A' && c <= u'Z') return c - u'A' + 10;
  return 99;
}

std::u16string_view trimStart(std::u16string_view s) {
  size_t i = 0;
  while (i < s.size() && isWhiteSpaceOrLineTerminator(s[i])) i++;
  return s.substr(i);
}

// 19.2.5 parseInt ( string, radix )
Result<Value> Global_parseInt(NativeCall& call) {
  String* inputString = TRY(ToString(call.args[0]));
  std::u16string_view s = trimStart(inputString->view());
  double sign = 1;
  if (!s.empty() && s[0] == u'-') sign = -1;
  if (!s.empty() && (s[0] == u'+' || s[0] == u'-')) s = s.substr(1);
  int32_t r = TRY(ToInt32(call.args[1]));
  bool stripPrefix = true;
  if (r != 0) {
    if (r < 2 || r > 36) return num(NAN);
    if (r != 16) stripPrefix = false;
  } else {
    r = 10;
  }
  if (stripPrefix && s.size() >= 2 && s[0] == u'0' && (s[1] == u'x' || s[1] == u'X')) {
    s = s.substr(2);
    r = 16;
  }
  size_t end = 0;
  while (end < s.size() && digitValue(s[end]) < r) end++;
  std::u16string_view z = s.substr(0, end);
  if (z.empty()) return num(NAN);
  double mathInt;
  if (r == 10) {
    std::string ascii(z.begin(), z.end());
    mathInt = std::strtod(ascii.c_str(), nullptr);
  } else {
    std::string ascii(z.begin(), z.end());
    mathInt = BigInt::fromDigits(ascii, r)->toDouble();
  }
  if (mathInt == 0) return num(sign < 0 ? -0.0 : 0.0);
  return num(sign * mathInt);
}

// 19.2.4 parseFloat ( string )
Result<Value> Global_parseFloat(NativeCall& call) {
  String* inputString = TRY(ToString(call.args[0]));
  std::u16string_view s = trimStart(inputString->view());
  size_t i = 0;
  bool negative = false;
  if (i < s.size() && (s[i] == u'+' || s[i] == u'-')) {
    negative = s[i] == u'-';
    i++;
  }
  if (s.substr(i).starts_with(u"Infinity")) return num(negative ? -INFINITY : INFINITY);
  size_t start = i;
  size_t digits = 0;
  while (i < s.size() && s[i] >= u'0' && s[i] <= u'9') i++, digits++;
  if (i < s.size() && s[i] == u'.') {
    size_t j = i + 1;
    size_t frac = 0;
    while (j < s.size() && s[j] >= u'0' && s[j] <= u'9') j++, frac++;
    if (digits + frac > 0) {
      i = j;
      digits += frac;
    }
  }
  if (digits == 0) return num(NAN);
  if (i < s.size() && (s[i] == u'e' || s[i] == u'E')) {
    size_t j = i + 1;
    if (j < s.size() && (s[j] == u'+' || s[j] == u'-')) j++;
    size_t expDigits = 0;
    while (j < s.size() && s[j] >= u'0' && s[j] <= u'9') j++, expDigits++;
    if (expDigits > 0) i = j;
  }
  std::string ascii(s.begin() + start, s.begin() + i);
  double value = std::strtod(ascii.c_str(), nullptr);
  return num(negative ? -value : value);
}

// 19.2.3 isNaN ( number ), 19.2.2 isFinite ( number )
Result<Value> Global_isNaN(NativeCall& call) { return boolean(std::isnan(TRY(ToNumber(call.args[0])))); }
Result<Value> Global_isFinite(NativeCall& call) { return boolean(std::isfinite(TRY(ToNumber(call.args[0])))); }

// 19.2.6.5 Encode ( string, extraUnescaped )
Result<Value> Encode(String* string, std::u16string_view extraUnescaped) {
  static const char16_t* alwaysUnescaped = u"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-.!~*'()_";
  std::u16string_view unescaped(alwaysUnescaped);
  std::u16string_view s = string->view();
  std::u16string r;
  static const char hex[] = "0123456789ABCDEF";
  for (size_t k = 0; k < s.size();) {
    char16_t c = s[k];
    if (unescaped.find(c) != std::u16string_view::npos || extraUnescaped.find(c) != std::u16string_view::npos) {
      r.push_back(c);
      k++;
      continue;
    }
    char32_t cp;
    if (c >= 0xDC00 && c <= 0xDFFF) return ThrowURIError("URI malformed");
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (k + 1 >= s.size() || s[k + 1] < 0xDC00 || s[k + 1] > 0xDFFF) return ThrowURIError("URI malformed");
      cp = 0x10000 + ((char32_t(c) - 0xD800) << 10) + (char32_t(s[k + 1]) - 0xDC00);
      k += 2;
    } else {
      cp = c;
      k++;
    }
    std::string octets;
    if (cp < 0x80) {
      octets.push_back(char(cp));
    } else if (cp < 0x800) {
      octets.push_back(char(0xC0 | (cp >> 6)));
      octets.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      octets.push_back(char(0xE0 | (cp >> 12)));
      octets.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      octets.push_back(char(0x80 | (cp & 0x3F)));
    } else {
      octets.push_back(char(0xF0 | (cp >> 18)));
      octets.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      octets.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      octets.push_back(char(0x80 | (cp & 0x3F)));
    }
    for (unsigned char o : octets) {
      r.push_back(u'%');
      r.push_back(char16_t(hex[o >> 4]));
      r.push_back(char16_t(hex[o & 15]));
    }
  }
  return Value(newString(std::move(r)));
}

int hexValue(char16_t c) {
  if (c >= u'0' && c <= u'9') return c - u'0';
  if (c >= u'a' && c <= u'f') return c - u'a' + 10;
  if (c >= u'A' && c <= u'F') return c - u'A' + 10;
  return -1;
}

// 19.2.6.6 Decode ( string, preserveEscapeSet )
Result<Value> Decode(String* string, std::u16string_view preserveEscapeSet) {
  std::u16string_view s = string->view();
  std::u16string r;
  size_t strLen = s.size();
  for (size_t k = 0; k < strLen; k++) {
    char16_t c = s[k];
    if (c != u'%') {
      r.push_back(c);
      continue;
    }
    size_t start = k;
    if (k + 2 >= strLen) return ThrowURIError("URI malformed");
    int h1 = hexValue(s[k + 1]), h2 = hexValue(s[k + 2]);
    if (h1 < 0 || h2 < 0) return ThrowURIError("URI malformed");
    unsigned b = unsigned(h1 * 16 + h2);
    k += 2;
    if (b < 0x80) {
      char16_t ch = char16_t(b);
      if (preserveEscapeSet.find(ch) == std::u16string_view::npos) r.push_back(ch);
      else r.append(s.substr(start, k - start + 1));
      continue;
    }
    int n;
    if ((b & 0xE0) == 0xC0) n = 2;
    else if ((b & 0xF0) == 0xE0) n = 3;
    else if ((b & 0xF8) == 0xF0) n = 4;
    else return ThrowURIError("URI malformed");
    if (k + 3 * (n - 1) >= strLen) return ThrowURIError("URI malformed");
    char32_t cp = b & (0xFF >> (n + 1));
    for (int j = 1; j < n; j++) {
      k++;
      if (s[k] != u'%') return ThrowURIError("URI malformed");
      int a1 = hexValue(s[k + 1]), a2 = hexValue(s[k + 2]);
      if (a1 < 0 || a2 < 0) return ThrowURIError("URI malformed");
      unsigned bb = unsigned(a1 * 16 + a2);
      if ((bb & 0xC0) != 0x80) return ThrowURIError("URI malformed");
      k += 2;
      cp = (cp << 6) | (bb & 0x3F);
    }
    // Reject overlong forms, surrogates and values above U+10FFFF.
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return ThrowURIError("URI malformed");
    if (cp < 0x10000) {
      r.push_back(char16_t(cp));
    } else {
      cp -= 0x10000;
      r.push_back(char16_t(0xD800 + (cp >> 10)));
      r.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
    }
  }
  return Value(newString(std::move(r)));
}

Result<Value> Global_decodeURI(NativeCall& call) {
  String* uriString = TRY(ToString(call.args[0]));
  return Decode(uriString, u";/?:@&=+$,#");
}
Result<Value> Global_decodeURIComponent(NativeCall& call) {
  String* s = TRY(ToString(call.args[0]));
  return Decode(s, u"");
}
Result<Value> Global_encodeURI(NativeCall& call) {
  String* uriString = TRY(ToString(call.args[0]));
  return Encode(uriString, u";/?:@&=+$,#");
}
Result<Value> Global_encodeURIComponent(NativeCall& call) {
  String* s = TRY(ToString(call.args[0]));
  return Encode(s, u"");
}

// B.2.1.1 escape ( string )
Result<Value> Global_escape(NativeCall& call) {
  String* string = TRY(ToString(call.args[0]));
  std::u16string_view unescapedSet = u"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789@*_+-./";
  static const char hex[] = "0123456789ABCDEF";
  std::u16string r;
  for (char16_t c : string->view()) {
    if (unescapedSet.find(c) != std::u16string_view::npos) {
      r.push_back(c);
    } else if (c < 256) {
      r.push_back(u'%');
      r.push_back(char16_t(hex[c >> 4]));
      r.push_back(char16_t(hex[c & 15]));
    } else {
      r.append(u"%u");
      r.push_back(char16_t(hex[(c >> 12) & 15]));
      r.push_back(char16_t(hex[(c >> 8) & 15]));
      r.push_back(char16_t(hex[(c >> 4) & 15]));
      r.push_back(char16_t(hex[c & 15]));
    }
  }
  return Value(newString(std::move(r)));
}

// B.2.1.2 unescape ( string )
Result<Value> Global_unescape(NativeCall& call) {
  String* string = TRY(ToString(call.args[0]));
  std::u16string_view s = string->view();
  std::u16string r;
  size_t len = s.size();
  for (size_t k = 0; k < len; k++) {
    char16_t c = s[k];
    if (c == u'%') {
      if (k + 6 <= len && s[k + 1] == u'u') {
        int a = hexValue(s[k + 2]), b = hexValue(s[k + 3]), cc = hexValue(s[k + 4]), d = hexValue(s[k + 5]);
        if (a >= 0 && b >= 0 && cc >= 0 && d >= 0) {
          r.push_back(char16_t((a << 12) | (b << 8) | (cc << 4) | d));
          k += 5;
          continue;
        }
      }
      if (k + 3 <= len) {
        int a = hexValue(s[k + 1]), b = hexValue(s[k + 2]);
        if (a >= 0 && b >= 0) {
          r.push_back(char16_t((a << 4) | b));
          k += 2;
          continue;
        }
      }
    }
    r.push_back(c);
  }
  return Value(newString(std::move(r)));
}

void installGlobal(Realm& realm) {
  Object* global = realm.globalObject();
  defineValue(global, PropertyKey(u"globalThis"), global, Attr::WritableConfigurable);
  defineValue(global, PropertyKey(u"Infinity"), num(INFINITY), Attr::None);
  defineValue(global, PropertyKey(u"NaN"), num(NAN), Attr::None);
  defineValue(global, PropertyKey(u"undefined"), Value::undefined(), Attr::None);

  struct Fn {
    const char16_t* name;
    int length;
    NativeFn fn;
    Intrinsic id;
  };
  const Fn fns[] = {
      {u"decodeURI", 1, Global_decodeURI, Intrinsic::decodeURI},
      {u"decodeURIComponent", 1, Global_decodeURIComponent, Intrinsic::decodeURIComponent},
      {u"encodeURI", 1, Global_encodeURI, Intrinsic::encodeURI},
      {u"encodeURIComponent", 1, Global_encodeURIComponent, Intrinsic::encodeURIComponent},
      {u"escape", 1, Global_escape, Intrinsic::escape},
      {u"isFinite", 1, Global_isFinite, Intrinsic::isFinite},
      {u"isNaN", 1, Global_isNaN, Intrinsic::isNaN},
      {u"parseFloat", 1, Global_parseFloat, Intrinsic::parseFloat},
      {u"parseInt", 2, Global_parseInt, Intrinsic::parseInt},
      {u"unescape", 1, Global_unescape, Intrinsic::unescape},
  };
  for (const Fn& f : fns) {
    BuiltinFunction* fn = defineFunction(realm, global, PropertyKey(f.name), f.length, f.fn);
    realm.setIntrinsic(f.id, fn);
  }
}

}  // namespace

SNOWMAN_BUILTIN(Global, kOrderFundamental, installGlobal);

}  // namespace snowman::builtins
