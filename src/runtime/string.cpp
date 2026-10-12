// Strings, the atom table and UTF-8/UTF-16 conversion.
#include "runtime/string.h"

#include <cstdint>
#include <unordered_map>

#include "unicode/unicode.h"

namespace snowman {

namespace {

size_t hashUnits(std::u16string_view units) {
  uint64_t h = 1469598103934665603ull;
  for (char16_t c : units) {
    h ^= uint64_t(c);
    h *= 1099511628211ull;
  }
  return size_t(h ? h : 1);
}

struct ViewHash {
  size_t operator()(std::u16string_view v) const { return hashUnits(v); }
};

std::unordered_map<std::u16string_view, String*, ViewHash>& atomTable() {
  static auto* table = new std::unordered_map<std::u16string_view, String*, ViewHash>();
  return *table;
}

void appendUtf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(char(cp));
  } else if (cp < 0x800) {
    out.push_back(char(0xC0 | (cp >> 6)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(char(0xE0 | (cp >> 12)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(char(0xF0 | (cp >> 18)));
    out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
}

}  // namespace

size_t String::hash() const {
  if (!hash_) hash_ = hashUnits(units_);
  return hash_;
}

std::string String::toUtf8() const { return utf16ToUtf8(units_); }

String* emptyString() {
  static String* empty = atom(std::u16string_view());
  return empty;
}

String* newString(std::u16string_view units) {
  if (units.empty()) return emptyString();
  return make<String>(std::u16string(units));
}

String* newString(std::u16string&& units) {
  if (units.empty()) return emptyString();
  return make<String>(std::move(units));
}

String* newStringFromUtf8(std::string_view utf8) { return newString(utf8ToUtf16(utf8)); }

String* atom(std::u16string_view units) {
  auto& table = atomTable();
  auto it = table.find(units);
  if (it != table.end()) return it->second;
  String* s = make<String>(std::u16string(units));
  s->atom_ = true;
  table.emplace(s->view(), s);
  return s;
}

String* atom(String* string) {
  if (string->isAtom()) return string;
  return atom(string->view());
}

String* atomFromUtf8(std::string_view utf8) { return atom(utf8ToUtf16(utf8)); }

bool equals(const String* a, const String* b) {
  if (a == b) return true;
  if (a->isAtom() && b->isAtom()) return false;
  return a->view() == b->view();
}

String* concat(String* a, String* b) {
  if (a->length() == 0) return b;
  if (b->length() == 0) return a;
  std::u16string units;
  units.reserve(a->length() + b->length());
  units.append(a->view());
  units.append(b->view());
  return make<String>(std::move(units));
}

bool isWhiteSpaceOrLineTerminator(char16_t c) {
  switch (c) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0xA0: case 0xFEFF:
    case 0x2028: case 0x2029:
      return true;
    default:
      return c > 0x7F && unicode::isSpaceSeparator(c);
  }
}

std::u16string utf8ToUtf16(std::string_view utf8) {
  std::u16string out;
  out.reserve(utf8.size());
  size_t i = 0, n = utf8.size();
  while (i < n) {
    unsigned char c = utf8[i];
    char32_t cp;
    size_t len;
    if (c < 0x80) {
      cp = c;
      len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      len = 4;
    } else {
      out.push_back(0xFFFD);
      i++;
      continue;
    }
    if (i + len > n) {
      out.push_back(0xFFFD);
      i++;
      continue;
    }
    bool ok = true;
    for (size_t k = 1; k < len; k++) {
      unsigned char cc = utf8[i + k];
      if ((cc & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) {
      out.push_back(0xFFFD);
      i++;
      continue;
    }
    i += len;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(char16_t(0xD800 + (cp >> 10)));
      out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
    } else {
      // Surrogates encoded in UTF-8 (WTF-8) come through as lone code units.
      out.push_back(char16_t(cp));
    }
  }
  return out;
}

std::string utf16ToUtf8(std::u16string_view utf16) {
  std::string out;
  out.reserve(utf16.size());
  for (size_t i = 0; i < utf16.size(); i++) {
    char16_t c = utf16[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < utf16.size() && utf16[i + 1] >= 0xDC00 && utf16[i + 1] <= 0xDFFF) {
      char32_t cp = 0x10000 + ((char32_t(c) - 0xD800) << 10) + (char32_t(utf16[i + 1]) - 0xDC00);
      appendUtf8(out, cp);
      i++;
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      appendUtf8(out, 0xFFFD);
    } else {
      appendUtf8(out, c);
    }
  }
  return out;
}

}  // namespace snowman
