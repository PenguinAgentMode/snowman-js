// The tokenizer (src/parser/lexer.h).
#include "parser/lexer.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "unicode/unicode.h"

namespace snowman::parser {

bool isLineTerminator(char32_t c) { return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029; }

bool isWhiteSpace(char32_t c) {
  if (c == ' ' || c == '\t' || c == 0x0B || c == 0x0C) return true;
  if (c < 0x80) return false;
  return c == 0xA0 || c == 0xFEFF || unicode::isSpaceSeparator(c);
}

bool isIdentifierStartCP(char32_t c) {
  if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_';
  return unicode::isIDStart(c);
}

bool isIdentifierPartCP(char32_t c) {
  if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '$' || c == '_';
  return c == 0x200C || c == 0x200D || unicode::isIDContinue(c);
}

namespace {

struct KeywordEntry {
  const char* text;
  K kw;
};

const KeywordEntry kKeywords[] = {
    {"await", K::Await},         {"break", K::Break},       {"case", K::Case},
    {"catch", K::Catch},         {"class", K::Class},       {"const", K::Const},
    {"continue", K::Continue},   {"debugger", K::Debugger}, {"default", K::Default},
    {"delete", K::Delete},       {"do", K::Do},             {"else", K::Else},
    {"enum", K::Enum},           {"export", K::Export},     {"extends", K::Extends},
    {"false", K::False},         {"finally", K::Finally},   {"for", K::For},
    {"function", K::Function},   {"if", K::If},             {"import", K::Import},
    {"in", K::In},               {"instanceof", K::Instanceof}, {"new", K::New},
    {"null", K::Null},           {"return", K::Return},     {"super", K::Super},
    {"switch", K::Switch},       {"this", K::This},         {"throw", K::Throw},
    {"true", K::True},           {"try", K::Try},           {"typeof", K::Typeof},
    {"var", K::Var},             {"void", K::Void},         {"while", K::While},
    {"with", K::With},           {"yield", K::Yield},       {"implements", K::Implements},
    {"interface", K::Interface}, {"let", K::Let},           {"package", K::Package},
    {"private", K::Private},     {"protected", K::Protected}, {"public", K::Public},
    {"static", K::Static},       {"as", K::As},             {"async", K::Async},
    {"from", K::From},           {"get", K::Get},           {"of", K::Of},
    {"set", K::Set},             {"target", K::Target},     {"meta", K::Meta},
    {"using", K::Using},         {"eval", K::Eval},         {"arguments", K::Arguments},
    {"constructor", K::Constructor}, {"prototype", K::Prototype}, {"__proto__", K::Proto},
};

int hexValue(char32_t c) {
  if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<int>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A' + 10);
  return -1;
}

bool isDecimalDigit(char32_t c) { return c >= '0' && c <= '9'; }

void appendCodePoint(std::u16string& out, char32_t cp) {
  if (cp >= 0x10000) {
    cp -= 0x10000;
    out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
    out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
  } else {
    out.push_back(static_cast<char16_t>(cp));
  }
}

// Digits (values) of a radix-2^bits integer -> the nearest double (ties to even).
double radixToDouble(const std::vector<int>& digits, int bits) {
  std::vector<int> bitsMsb;
  bitsMsb.reserve(digits.size() * bits);
  for (int d : digits) {
    for (int b = bits - 1; b >= 0; b--) bitsMsb.push_back((d >> b) & 1);
  }
  size_t first = 0;
  while (first < bitsMsb.size() && bitsMsb[first] == 0) first++;
  size_t count = bitsMsb.size() - first;
  if (count == 0) return 0;
  if (count <= 53) {
    uint64_t m = 0;
    for (size_t i = first; i < bitsMsb.size(); i++) m = (m << 1) | static_cast<uint64_t>(bitsMsb[i]);
    return static_cast<double>(m);
  }
  uint64_t m = 0;
  for (size_t i = first; i < first + 53; i++) m = (m << 1) | static_cast<uint64_t>(bitsMsb[i]);
  int guard = bitsMsb[first + 53];
  bool sticky = false;
  for (size_t i = first + 54; i < bitsMsb.size(); i++) sticky = sticky || bitsMsb[i];
  int exponent = static_cast<int>(count - 53);
  if (guard && (sticky || (m & 1))) {
    m++;
    if (m == (uint64_t(1) << 53)) {
      m >>= 1;
      exponent++;
    }
  }
  return std::ldexp(static_cast<double>(m), exponent);
}

// Digits (values) in `radix` -> decimal digit string.
std::string radixToDecimal(const std::vector<int>& digits, int radix) {
  std::vector<uint32_t> limbs;  // little-endian base 1e9
  for (int d : digits) {
    uint64_t carry = static_cast<uint64_t>(d);
    for (auto& limb : limbs) {
      uint64_t v = static_cast<uint64_t>(limb) * static_cast<uint64_t>(radix) + carry;
      limb = static_cast<uint32_t>(v % 1000000000u);
      carry = v / 1000000000u;
    }
    while (carry) {
      limbs.push_back(static_cast<uint32_t>(carry % 1000000000u));
      carry /= 1000000000u;
    }
  }
  if (limbs.empty()) return "0";
  std::string out = std::to_string(limbs.back());
  for (size_t i = limbs.size() - 1; i-- > 0;) {
    std::string part = std::to_string(limbs[i]);
    out += std::string(9 - part.size(), '0') + part;
  }
  return out;
}

}  // namespace

K keywordOf(std::u16string_view name) {
  if (name.size() < 2 || name.size() > 11) return K::None;
  for (const auto& entry : kKeywords) {
    size_t n = std::strlen(entry.text);
    if (n != name.size()) continue;
    bool same = true;
    for (size_t i = 0; i < n; i++) {
      if (name[i] != static_cast<char16_t>(entry.text[i])) {
        same = false;
        break;
      }
    }
    if (same) return entry.kw;
  }
  return K::None;
}

Lexer::Lexer(const char16_t* src, uint32_t length, bool module, bool hashbang)
    : src_(src), len_(length), module_(module) {
  if (hashbang && len_ >= 2 && src_[0] == '#' && src_[1] == '!') {
    pos_ = 2;
    while (pos_ < len_ && !isLineTerminator(src_[pos_])) pos_++;
  }
}

void Lexer::lineColumnOf(uint32_t pos, uint32_t& line, uint32_t& column) const {
  line = 1;
  uint32_t start = 0;
  for (uint32_t i = 0; i < pos && i < len_; i++) {
    char16_t c = src_[i];
    if (c == '\r' && i + 1 < len_ && src_[i + 1] == '\n') continue;
    if (isLineTerminator(c)) {
      line++;
      start = i + 1;
    }
  }
  column = pos - start;
}

void Lexer::fail(uint32_t pos, const std::string& message) const {
  ParseError error;
  error.message = message;
  lineColumnOf(pos, error.line, error.column);
  throw SyntaxErrorException{error};
}

char32_t Lexer::peekCodePoint(uint32_t at, uint32_t& size) const {
  if (at >= len_) {
    size = 0;
    return 0xFFFFFFFF;
  }
  char16_t c = src_[at];
  if (c >= 0xD800 && c <= 0xDBFF && at + 1 < len_ && src_[at + 1] >= 0xDC00 && src_[at + 1] <= 0xDFFF) {
    size = 2;
    return 0x10000 + ((c - 0xD800) << 10) + (src_[at + 1] - 0xDC00);
  }
  size = 1;
  return c;
}

void Lexer::newline(uint32_t at) {
  (void)at;
  line_++;
  lineStart_ = pos_;
}

void Lexer::skipTrivia(Token& tok) {
  bool lineStart = pos_ == 0;
  tok.nlBefore = false;
  while (pos_ < len_) {
    char16_t c = src_[pos_];
    if (c == '\n' || c == 0x2028 || c == 0x2029) {
      pos_++;
      newline(pos_ - 1);
      tok.nlBefore = lineStart = true;
      continue;
    }
    if (c == '\r') {
      pos_++;
      if (pos_ < len_ && src_[pos_] == '\n') pos_++;
      newline(pos_ - 1);
      tok.nlBefore = lineStart = true;
      continue;
    }
    if (c == ' ' || c == '\t') {
      pos_++;
      continue;
    }
    if (c == '/' && pos_ + 1 < len_) {
      if (src_[pos_ + 1] == '/') {
        pos_ += 2;
        while (pos_ < len_ && !isLineTerminator(src_[pos_])) pos_++;
        continue;
      }
      if (src_[pos_ + 1] == '*') {
        uint32_t start = pos_;
        pos_ += 2;
        bool closed = false;
        while (pos_ < len_) {
          char16_t d = src_[pos_];
          if (d == '*' && pos_ + 1 < len_ && src_[pos_ + 1] == '/') {
            pos_ += 2;
            closed = true;
            break;
          }
          pos_++;
          if (d == '\r' && pos_ < len_ && src_[pos_] == '\n') pos_++;
          if (isLineTerminator(d)) {
            newline(pos_ - 1);
            tok.nlBefore = lineStart = true;
          }
        }
        if (!closed) fail(start, "Unterminated comment");
        continue;
      }
      break;
    }
    if (!module_) {
      // Annex B.1.1 HTML-like comments.
      if (c == '<' && pos_ + 3 < len_ && src_[pos_ + 1] == '!' && src_[pos_ + 2] == '-' && src_[pos_ + 3] == '-') {
        pos_ += 4;
        while (pos_ < len_ && !isLineTerminator(src_[pos_])) pos_++;
        continue;
      }
      if (c == '-' && lineStart && pos_ + 2 < len_ && src_[pos_ + 1] == '-' && src_[pos_ + 2] == '>') {
        pos_ += 3;
        while (pos_ < len_ && !isLineTerminator(src_[pos_])) pos_++;
        continue;
      }
    }
    if (c >= 0x80 || c == 0x0B || c == 0x0C) {
      uint32_t size;
      char32_t cp = peekCodePoint(pos_, size);
      if (isWhiteSpace(cp)) {
        pos_ += size;
        continue;
      }
    }
    break;
  }
}

void Lexer::next(Token& tok) {
  skipTrivia(tok);
  tok.escaped = false;
  tok.legacyOctal = false;
  tok.templateTail = false;
  tok.cookedInvalid = false;
  tok.octalEscape = -1;
  tok.kw = K::None;
  tok.start = pos_;
  tok.line = line_;
  tok.col = pos_ - lineStart_;
  if (pos_ >= len_) {
    tok.type = T::EOS;
    tok.end = pos_;
    return;
  }
  char16_t c = src_[pos_];
  if (c < 0x80) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_' || c == '\\') {
      lexIdentifier(tok, false);
      return;
    }
    if (c >= '0' && c <= '9') {
      lexNumber(tok);
      return;
    }
    if (c == '.' && pos_ + 1 < len_ && isDecimalDigit(src_[pos_ + 1])) {
      lexNumber(tok);
      return;
    }
    if (c == '"' || c == '\'') {
      lexString(tok, c);
      return;
    }
    if (c == '`') {
      lexTemplate(tok, pos_ + 1);
      return;
    }
    if (c == '#') {
      lexIdentifier(tok, true);
      return;
    }
    lexPunctuator(tok);
    return;
  }
  uint32_t size;
  char32_t cp = peekCodePoint(pos_, size);
  if (isIdentifierStartCP(cp)) {
    lexIdentifier(tok, false);
    return;
  }
  fail(pos_, "Invalid or unexpected token");
}

char32_t Lexer::lexUnicodeEscape(uint32_t& p, bool& ok) {
  // p is at the 'u' after a backslash
  ok = false;
  if (p >= len_ || src_[p] != 'u') return 0;
  p++;
  if (p < len_ && src_[p] == '{') {
    p++;
    char32_t value = 0;
    int digits = 0;
    while (p < len_ && hexValue(src_[p]) >= 0) {
      value = value * 16 + static_cast<char32_t>(hexValue(src_[p]));
      if (value > 0x10FFFF) return 0;
      p++;
      digits++;
    }
    if (digits == 0 || p >= len_ || src_[p] != '}') return 0;
    p++;
    ok = true;
    return value;
  }
  char32_t value = 0;
  for (int i = 0; i < 4; i++) {
    if (p >= len_ || hexValue(src_[p]) < 0) return 0;
    value = value * 16 + static_cast<char32_t>(hexValue(src_[p]));
    p++;
  }
  ok = true;
  return value;
}

void Lexer::lexIdentifier(Token& tok, bool privateName) {
  uint32_t p = pos_;
  if (privateName) p++;
  tok.str.clear();
  bool first = true;
  while (p < len_) {
    char16_t c = src_[p];
    if (c < 0x80 && c != '\\') {
      bool ok = first ? isIdentifierStartCP(c) : isIdentifierPartCP(c);
      if (!ok) break;
      tok.str.push_back(c);
      p++;
      first = false;
      continue;
    }
    if (c == '\\') {
      uint32_t escStart = p;
      p++;
      bool ok;
      char32_t cp = lexUnicodeEscape(p, ok);
      if (!ok) fail(escStart, "Invalid Unicode escape sequence");
      bool valid = first ? isIdentifierStartCP(cp) : isIdentifierPartCP(cp);
      if (!valid) fail(escStart, "Invalid Unicode escape sequence in identifier");
      appendCodePoint(tok.str, cp);
      tok.escaped = true;
      first = false;
      continue;
    }
    uint32_t size;
    char32_t cp = peekCodePoint(p, size);
    bool ok = first ? isIdentifierStartCP(cp) : isIdentifierPartCP(cp);
    if (!ok) break;
    appendCodePoint(tok.str, cp);
    p += size;
    first = false;
  }
  if (first) fail(pos_, privateName ? "Invalid character after #" : "Invalid or unexpected token");
  tok.type = privateName ? T::PrivateName : T::Name;
  tok.kw = privateName ? K::None : keywordOf(tok.str);
  pos_ = p;
  tok.end = p;
}

void Lexer::lexNumber(Token& tok) {
  uint32_t start = pos_;
  uint32_t p = pos_;
  auto at = [&](uint32_t i) -> char16_t { return i < len_ ? src_[i] : 0; };
  // Reads digits accepted by `digitOk`, with numeric separators between digits when `separators`.
  auto readDigits = [&](auto digitOk, bool separators, std::vector<int>* values, std::string* text) -> int {
    int count = 0;
    bool lastSeparator = false;
    while (p < len_) {
      char16_t c = src_[p];
      if (c == '_' && separators) {
        if (count == 0 || lastSeparator) fail(p, "Numeric separators are not allowed here");
        lastSeparator = true;
        p++;
        continue;
      }
      int v = digitOk(c);
      if (v < 0) break;
      if (values) values->push_back(v);
      if (text) text->push_back(static_cast<char>(c));
      lastSeparator = false;
      count++;
      p++;
    }
    if (lastSeparator) fail(p - 1, "Numeric separators are not allowed at the end of numeric literals");
    return count;
  };
  auto decimal = [](char16_t c) { return isDecimalDigit(c) ? static_cast<int>(c - '0') : -1; };
  tok.type = T::Num;
  bool isBigInt = false;

  char16_t c0 = at(p), c1 = at(p + 1);
  if (c0 == '0' && (c1 == 'x' || c1 == 'X' || c1 == 'o' || c1 == 'O' || c1 == 'b' || c1 == 'B')) {
    int bits = (c1 == 'x' || c1 == 'X') ? 4 : (c1 == 'o' || c1 == 'O') ? 3 : 1;
    int radix = 1 << bits;
    p += 2;
    std::vector<int> values;
    auto digitOk = [radix](char16_t c) {
      int v = hexValue(c);
      return v >= 0 && v < radix ? v : -1;
    };
    if (readDigits(digitOk, true, &values, nullptr) == 0) fail(start, "Invalid or unexpected token");
    if (at(p) == 'n') {
      p++;
      isBigInt = true;
      tok.bigint = radixToDecimal(values, radix);
    } else {
      tok.num = radixToDouble(values, bits);
    }
  } else if (c0 == '0' && isDecimalDigit(c1)) {
    // LegacyOctalIntegerLiteral or NonOctalDecimalIntegerLiteral.
    tok.legacyOctal = true;
    uint32_t q = p + 1;
    bool octal = true;
    while (q < len_ && isDecimalDigit(src_[q])) {
      if (src_[q] >= '8') octal = false;
      q++;
    }
    if (octal) {
      std::vector<int> values;
      for (uint32_t i = p + 1; i < q; i++) values.push_back(src_[i] - '0');
      p = q;
      tok.num = radixToDouble(values, 3);
      if (at(p) == '_') fail(p, "Numeric separators are not allowed in legacy octal literals");
    } else {
      std::string text;
      for (uint32_t i = p; i < q; i++) text.push_back(static_cast<char>(src_[i]));
      p = q;
      if (at(p) == '_') fail(p, "Numeric separators are not allowed here");
      if (at(p) == '.') {
        text.push_back('.');
        p++;
        readDigits(decimal, true, nullptr, &text);
      }
      if (at(p) == 'e' || at(p) == 'E') {
        text.push_back('e');
        p++;
        if (at(p) == '+' || at(p) == '-') text.push_back(static_cast<char>(src_[p++]));
        if (readDigits(decimal, true, nullptr, &text) == 0) fail(start, "Invalid or unexpected token");
      }
      tok.num = std::strtod(text.c_str(), nullptr);
    }
  } else {
    std::string text;
    bool integer = true;
    if (c0 != '.') {
      if (c0 == '0' && c1 == '_') fail(p + 1, "Numeric separator can not be used after leading 0");
      readDigits(decimal, true, nullptr, &text);
    }
    if (at(p) == '.') {
      integer = false;
      text.push_back('.');
      p++;
      if (at(p) == '_') fail(p, "Numeric separators are not allowed here");
      readDigits(decimal, true, nullptr, &text);
    }
    if (at(p) == 'e' || at(p) == 'E') {
      integer = false;
      text.push_back('e');
      p++;
      if (at(p) == '+' || at(p) == '-') text.push_back(static_cast<char>(src_[p++]));
      if (at(p) == '_') fail(p, "Numeric separators are not allowed here");
      if (readDigits(decimal, true, nullptr, &text) == 0) fail(start, "Invalid or unexpected token");
    }
    if (at(p) == 'n' && integer) {
      p++;
      isBigInt = true;
      size_t nz = 0;
      while (nz + 1 < text.size() && text[nz] == '0') nz++;
      tok.bigint = text.substr(nz);
    } else {
      tok.num = std::strtod(text.c_str(), nullptr);
    }
  }
  if (isBigInt) tok.type = T::BigInt;
  // The SourceCharacter immediately following a NumericLiteral must not be an IdentifierStart or DecimalDigit.
  uint32_t size;
  char32_t next = peekCodePoint(p, size);
  if (size && (next == '\\' || isDecimalDigit(next) || isIdentifierStartCP(next)))
    fail(p, "Invalid or unexpected token");
  pos_ = p;
  tok.end = p;
}

void Lexer::lexString(Token& tok, char16_t quote) {
  uint32_t p = pos_ + 1;
  tok.str.clear();
  tok.type = T::Str;
  while (true) {
    if (p >= len_) fail(pos_, "Invalid or unexpected token");
    char16_t c = src_[p];
    if (c == quote) {
      p++;
      break;
    }
    if (c == '\n' || c == '\r') fail(pos_, "Invalid or unexpected token");
    if (c != '\\') {
      tok.str.push_back(c);
      p++;
      continue;
    }
    uint32_t escStart = p;
    p++;
    if (p >= len_) fail(pos_, "Invalid or unexpected token");
    char16_t e = src_[p];
    switch (e) {
      case '\r':
        p++;
        if (p < len_ && src_[p] == '\n') p++;
        pos_ = p;
        newline(p - 1);
        break;
      case '\n':
      case 0x2028:
      case 0x2029:
        p++;
        pos_ = p;
        newline(p - 1);
        break;
      case 'n': tok.str.push_back('\n'); p++; break;
      case 't': tok.str.push_back('\t'); p++; break;
      case 'r': tok.str.push_back('\r'); p++; break;
      case 'b': tok.str.push_back('\b'); p++; break;
      case 'f': tok.str.push_back('\f'); p++; break;
      case 'v': tok.str.push_back('\v'); p++; break;
      case 'x': {
        p++;
        int h1 = p < len_ ? hexValue(src_[p]) : -1;
        int h2 = p + 1 < len_ ? hexValue(src_[p + 1]) : -1;
        if (h1 < 0 || h2 < 0) fail(escStart, "Invalid hexadecimal escape sequence");
        tok.str.push_back(static_cast<char16_t>(h1 * 16 + h2));
        p += 2;
        break;
      }
      case 'u': {
        bool ok;
        char32_t cp = lexUnicodeEscape(p, ok);
        if (!ok) fail(escStart, "Invalid Unicode escape sequence");
        appendCodePoint(tok.str, cp);
        break;
      }
      default:
        if (e >= '0' && e <= '7') {
          if (e == '0' && !(p + 1 < len_ && isDecimalDigit(src_[p + 1]))) {
            tok.str.push_back(0);
            p++;
            break;
          }
          if (tok.octalEscape < 0) tok.octalEscape = static_cast<int32_t>(escStart);
          int value = e - '0';
          p++;
          int maxDigits = e <= '3' ? 3 : 2;
          for (int i = 1; i < maxDigits && p < len_ && src_[p] >= '0' && src_[p] <= '7'; i++) {
            value = value * 8 + (src_[p] - '0');
            p++;
          }
          tok.str.push_back(static_cast<char16_t>(value));
          break;
        }
        if (e == '8' || e == '9') {
          if (tok.octalEscape < 0) tok.octalEscape = static_cast<int32_t>(escStart);
          tok.str.push_back(e);
          p++;
          break;
        }
        tok.str.push_back(e);
        p++;
        break;
    }
  }
  pos_ = p;
  tok.end = p;
}

void Lexer::lexTemplate(Token& tok, uint32_t from) {
  // `from` is just past the opening ` or the } that closes a substitution.
  uint32_t p = from;
  tok.type = T::Template;
  tok.str.clear();
  tok.raw.clear();
  tok.cookedInvalid = false;
  while (true) {
    if (p >= len_) fail(tok.start, "Unterminated template literal");
    char16_t c = src_[p];
    if (c == '`') {
      p++;
      tok.templateTail = true;
      break;
    }
    if (c == '$' && p + 1 < len_ && src_[p + 1] == '{') {
      p += 2;
      tok.templateTail = false;
      break;
    }
    if (c == '\r') {
      p++;
      if (p < len_ && src_[p] == '\n') p++;
      tok.str.push_back('\n');
      tok.raw.push_back('\n');
      pos_ = p;
      newline(p - 1);
      continue;
    }
    if (c == '\n' || c == 0x2028 || c == 0x2029) {
      p++;
      tok.str.push_back(c);
      tok.raw.push_back(c);
      pos_ = p;
      newline(p - 1);
      continue;
    }
    if (c != '\\') {
      tok.str.push_back(c);
      tok.raw.push_back(c);
      p++;
      continue;
    }
    uint32_t escStart = p;
    p++;
    if (p >= len_) fail(tok.start, "Unterminated template literal");
    char16_t e = src_[p];
    tok.raw.push_back('\\');
    auto invalid = [&]() {
      if (!tok.cookedInvalid) tok.invalidEscapePos = escStart;
      tok.cookedInvalid = true;
    };
    switch (e) {
      case '\r':
        p++;
        if (p < len_ && src_[p] == '\n') p++;
        tok.raw.push_back('\n');
        pos_ = p;
        newline(p - 1);
        continue;
      case '\n':
      case 0x2028:
      case 0x2029:
        p++;
        tok.raw.push_back(e);
        pos_ = p;
        newline(p - 1);
        continue;
      case 'n': tok.str.push_back('\n'); break;
      case 't': tok.str.push_back('\t'); break;
      case 'r': tok.str.push_back('\r'); break;
      case 'b': tok.str.push_back('\b'); break;
      case 'f': tok.str.push_back('\f'); break;
      case 'v': tok.str.push_back('\v'); break;
      case 'x': {
        int h1 = p + 1 < len_ ? hexValue(src_[p + 1]) : -1;
        int h2 = p + 2 < len_ ? hexValue(src_[p + 2]) : -1;
        if (h1 < 0 || h2 < 0) {
          invalid();
          break;
        }
        tok.str.push_back(static_cast<char16_t>(h1 * 16 + h2));
        tok.raw.push_back('x');
        tok.raw.push_back(src_[p + 1]);
        tok.raw.push_back(src_[p + 2]);
        p += 3;
        continue;
      }
      case 'u': {
        uint32_t q = p;
        bool ok;
        char32_t cp = lexUnicodeEscape(q, ok);
        if (!ok) {
          invalid();
          break;
        }
        appendCodePoint(tok.str, cp);
        tok.raw.append(src_ + p, q - p);
        p = q;
        continue;
      }
      default:
        if (e == '0' && !(p + 1 < len_ && isDecimalDigit(src_[p + 1]))) {
          tok.str.push_back(0);
          break;
        }
        if (isDecimalDigit(e)) {
          invalid();
          break;
        }
        tok.str.push_back(e);
        break;
    }
    // A one-character escape (or the first character of an invalid one).
    tok.raw.push_back(e);
    p++;
  }
  pos_ = p;
  tok.end = p;
}

void Lexer::rescanTemplate(Token& tok) {
  // tok is the `}`; the template continues right after it.
  pos_ = tok.start;
  lexTemplate(tok, tok.start + 1);
}

void Lexer::rescanRegExp(Token& tok) {
  uint32_t p = tok.start + 1;
  bool inClass = false;
  while (true) {
    if (p >= len_ || isLineTerminator(src_[p])) fail(tok.start, "Invalid regular expression: missing /");
    char16_t c = src_[p];
    if (c == '\\') {
      p++;
      if (p >= len_ || isLineTerminator(src_[p])) fail(tok.start, "Invalid regular expression: missing /");
      p++;
      continue;
    }
    if (c == '[') {
      inClass = true;
    } else if (c == ']') {
      inClass = false;
    } else if (c == '/' && !inClass) {
      break;
    }
    p++;
  }
  tok.str.assign(src_ + tok.start + 1, p - tok.start - 1);
  p++;
  uint32_t flagsStart = p;
  while (p < len_) {
    if (src_[p] == '\\') fail(p, "Invalid regular expression flags");
    uint32_t size;
    char32_t cp = peekCodePoint(p, size);
    if (!isIdentifierPartCP(cp)) break;
    p += size;
  }
  tok.raw.assign(src_ + flagsStart, p - flagsStart);
  tok.type = T::RegExp;
  pos_ = p;
  tok.end = p;
}

void Lexer::lexPunctuator(Token& tok) {
  uint32_t p = pos_;
  auto at = [&](uint32_t i) -> char16_t { return i < len_ ? src_[i] : 0; };
  char16_t c = src_[p];
  T type;
  uint32_t n = 1;
  switch (c) {
    case '{': type = T::LBrace; break;
    case '}': type = T::RBrace; break;
    case '(': type = T::LParen; break;
    case ')': type = T::RParen; break;
    case '[': type = T::LBracket; break;
    case ']': type = T::RBracket; break;
    case ';': type = T::Semi; break;
    case ',': type = T::Comma; break;
    case ':': type = T::Colon; break;
    case '~': type = T::Tilde; break;
    case '.':
      if (at(p + 1) == '.' && at(p + 2) == '.') {
        type = T::Ellipsis;
        n = 3;
      } else {
        type = T::Dot;
      }
      break;
    case '?':
      if (at(p + 1) == '?') {
        if (at(p + 2) == '=') {
          type = T::NullishEq;
          n = 3;
        } else {
          type = T::Nullish;
          n = 2;
        }
      } else if (at(p + 1) == '.' && !isDecimalDigit(at(p + 2))) {
        type = T::QuestionDot;
        n = 2;
      } else {
        type = T::Question;
      }
      break;
    case '<':
      if (at(p + 1) == '<') {
        if (at(p + 2) == '=') {
          type = T::ShlEq;
          n = 3;
        } else {
          type = T::Shl;
          n = 2;
        }
      } else if (at(p + 1) == '=') {
        type = T::LtEq;
        n = 2;
      } else {
        type = T::Lt;
      }
      break;
    case '>':
      if (at(p + 1) == '>') {
        if (at(p + 2) == '>') {
          if (at(p + 3) == '=') {
            type = T::ShrEq;
            n = 4;
          } else {
            type = T::Shr;
            n = 3;
          }
        } else if (at(p + 2) == '=') {
          type = T::SarEq;
          n = 3;
        } else {
          type = T::Sar;
          n = 2;
        }
      } else if (at(p + 1) == '=') {
        type = T::GtEq;
        n = 2;
      } else {
        type = T::Gt;
      }
      break;
    case '=':
      if (at(p + 1) == '=') {
        if (at(p + 2) == '=') {
          type = T::Eq3;
          n = 3;
        } else {
          type = T::Eq2;
          n = 2;
        }
      } else if (at(p + 1) == '>') {
        type = T::Arrow;
        n = 2;
      } else {
        type = T::Assign;
      }
      break;
    case '!':
      if (at(p + 1) == '=') {
        if (at(p + 2) == '=') {
          type = T::NotEq2;
          n = 3;
        } else {
          type = T::NotEq;
          n = 2;
        }
      } else {
        type = T::Bang;
      }
      break;
    case '+':
      if (at(p + 1) == '+') {
        type = T::Inc;
        n = 2;
      } else if (at(p + 1) == '=') {
        type = T::PlusEq;
        n = 2;
      } else {
        type = T::Plus;
      }
      break;
    case '-':
      if (at(p + 1) == '-') {
        type = T::Dec;
        n = 2;
      } else if (at(p + 1) == '=') {
        type = T::MinusEq;
        n = 2;
      } else {
        type = T::Minus;
      }
      break;
    case '*':
      if (at(p + 1) == '*') {
        if (at(p + 2) == '=') {
          type = T::Star2Eq;
          n = 3;
        } else {
          type = T::Star2;
          n = 2;
        }
      } else if (at(p + 1) == '=') {
        type = T::StarEq;
        n = 2;
      } else {
        type = T::Star;
      }
      break;
    case '/':
      if (at(p + 1) == '=') {
        type = T::SlashEq;
        n = 2;
      } else {
        type = T::Slash;
      }
      break;
    case '%':
      if (at(p + 1) == '=') {
        type = T::PercentEq;
        n = 2;
      } else {
        type = T::Percent;
      }
      break;
    case '&':
      if (at(p + 1) == '&') {
        if (at(p + 2) == '=') {
          type = T::AndEq;
          n = 3;
        } else {
          type = T::And;
          n = 2;
        }
      } else if (at(p + 1) == '=') {
        type = T::AmpEq;
        n = 2;
      } else {
        type = T::Amp;
      }
      break;
    case '|':
      if (at(p + 1) == '|') {
        if (at(p + 2) == '=') {
          type = T::OrEq;
          n = 3;
        } else {
          type = T::Or;
          n = 2;
        }
      } else if (at(p + 1) == '=') {
        type = T::PipeEq;
        n = 2;
      } else {
        type = T::Pipe;
      }
      break;
    case '^':
      if (at(p + 1) == '=') {
        type = T::CaretEq;
        n = 2;
      } else {
        type = T::Caret;
      }
      break;
    default:
      fail(p, "Invalid or unexpected token");
  }
  tok.type = type;
  pos_ = p + n;
  tok.end = pos_;
}

}  // namespace snowman::parser
