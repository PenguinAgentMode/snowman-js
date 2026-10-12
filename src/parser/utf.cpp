// UTF-8 <-> UTF-16 for the parser's callers (src/parser/parser.h).
#include "parser/parser.h"

namespace snowman::parser {

std::u16string utf8ToUtf16(std::string_view in) {
  std::u16string out;
  out.reserve(in.size());
  size_t i = 0, n = in.size();
  auto cont = [&](size_t k) { return k < n && (static_cast<unsigned char>(in[k]) & 0xC0) == 0x80; };
  while (i < n) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    if (c < 0x80) {
      out.push_back(c);
      i++;
      continue;
    }
    char32_t cp = 0xFFFD;
    size_t len = 1;
    if (c >= 0xC2 && c <= 0xDF && cont(i + 1)) {
      cp = ((c & 0x1F) << 6) | (in[i + 1] & 0x3F);
      len = 2;
    } else if (c >= 0xE0 && c <= 0xEF && cont(i + 1) && cont(i + 2)) {
      cp = ((c & 0x0F) << 12) | ((in[i + 1] & 0x3F) << 6) | (in[i + 2] & 0x3F);
      if (cp < 0x800) cp = 0xFFFD;
      len = 3;
    } else if (c >= 0xF0 && c <= 0xF4 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
      cp = ((c & 0x07) << 18) | ((in[i + 1] & 0x3F) << 12) | ((in[i + 2] & 0x3F) << 6) | (in[i + 3] & 0x3F);
      if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
      len = 4;
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
    i += len;
  }
  return out;
}

std::string utf16ToUtf8(std::u16string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    char32_t cp = in[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < in.size() && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
      i++;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

}  // namespace snowman::parser
