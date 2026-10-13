// The RegExp pattern parser: ECMA-262 2025 22.2.1 (Patterns, with their early errors) and Annex B.1.2 (the
// web-compatible grammar without the u and v flags). One recursive descent serves all three grammars; the
// flags select the productions.
#include <algorithm>
#include <utility>

#include "regexp/ast.h"

namespace snowman::regexp {
namespace {

constexpr char32_t kEnd = 0xFFFFFFFF;  // peek() past the end
// Nested v-mode classes are parsed (and evaluated by the compiler) recursively: their nesting is capped. Groups
// nest without recursion.
constexpr int kMaxClassDepth = 64;

bool isDecimalDigit(char32_t c) { return c >= '0' && c <= '9'; }
bool isOctalDigit(char32_t c) { return c >= '0' && c <= '7'; }
bool isHexDigit(char32_t c) { return isDecimalDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
uint32_t hexValue(char32_t c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }
bool isAsciiLetter(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isLead(char32_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool isTrail(char32_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
char32_t combineSurrogates(char32_t lead, char32_t trail) { return 0x10000 + ((lead - 0xD800) << 10) + (trail - 0xDC00); }
bool isOneOf(char32_t c, std::u32string_view set) { return c < 0x80 && set.find(c) != std::u32string_view::npos; }
bool isSyntaxCharacter(char32_t c) { return isOneOf(c, U"^$\\.*+?()[]{}|"); }
bool isClassSetSyntaxCharacter(char32_t c) { return isOneOf(c, U"()[]{}/-\\|"); }
bool isClassSetReservedPunctuator(char32_t c) { return isOneOf(c, U"&-!#%,:;<=>@`~"); }
// The characters that, doubled, are a ClassSetReservedDoublePunctuator.
bool isClassSetDoublePunctuatorChar(char32_t c) { return isOneOf(c, U"&!#$%*+,.:;<=>?@^`~"); }

std::u16string toUtf16(std::u32string_view s) {
  std::u16string out;
  for (char32_t c : s) {
    if (c >= 0x10000) {
      out += static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10));
      out += static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF));
    } else {
      out += static_cast<char16_t>(c);
    }
  }
  return out;
}

void appendUtf8(std::string& out, char32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

std::string toUtf8(std::u16string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    char32_t c = s[i];
    if (isLead(c) && i + 1 < s.size() && isTrail(s[i + 1])) {
      c = combineSurrogates(c, s[++i]);
    } else if (isLead(c) || isTrail(c)) {
      c = 0xFFFD;
    }
    appendUtf8(out, c);
  }
  return out;
}

// The pattern as the grammar sees it: code points with u or v (surrogate pairs joined), code units otherwise.
std::u32string decodePattern(std::u16string_view source, bool unicodeMode) {
  std::u32string s;
  s.reserve(source.size());
  for (size_t i = 0; i < source.size(); i++) {
    char32_t c = source[i];
    if (unicodeMode && isLead(c) && i + 1 < source.size() && isTrail(source[i + 1])) c = combineSurrogates(c, source[++i]);
    s += c;
  }
  return s;
}

// CountLeftCapturingParensWithin, and whether the pattern contains a GroupName (Annex B ParsePattern), found
// before parsing: in Annex B patterns the meaning of \N depends on the total count.
struct CaptureScan {
  uint32_t count = 0;
  bool hasNames = false;
};
CaptureScan scanCaptures(const std::u32string& s, bool unicodeSets) {
  CaptureScan scan;
  int classDepth = 0;
  for (size_t i = 0; i < s.size(); i++) {
    char32_t c = s[i];
    if (c == '\\') {
      i++;
    } else if (classDepth > 0) {
      if (c == ']') {
        classDepth--;
      } else if (c == '[' && unicodeSets) {
        classDepth++;
      }
    } else if (c == '[') {
      classDepth = 1;
    } else if (c == '(') {
      if (i + 1 < s.size() && s[i + 1] == '?') {
        if (i + 3 < s.size() && s[i + 2] == '<' && s[i + 3] != '=' && s[i + 3] != '!') {
          scan.count++;
          scan.hasNames = true;
        }
      } else {
        scan.count++;
      }
    }
  }
  return scan;
}

// The MV of a DecimalDigits run, saturated: QuantifierPrefix bounds and DecimalEscape numbers.
struct Digits {
  uint32_t value = 0;
  std::u32string_view text;  // without leading zeros, for exact comparison
};
bool greaterThan(const Digits& a, const Digits& b) {
  if (a.text.size() != b.text.size()) return a.text.size() > b.text.size();
  return a.text > b.text;
}

class Parser {
 public:
  Parser(Pattern& pattern, std::u32string source, bool namedGroups, uint32_t totalCaptures)
      : p_(pattern),
        s_(std::move(source)),
        u_(pattern.flags.unicode || pattern.flags.unicodeSets),
        v_(pattern.flags.unicodeSets),
        named_(namedGroups),
        totalCaptures_(totalCaptures) {
    if (p_.flags.ignoreCase) mods_ |= kIgnoreCase;
    if (p_.flags.multiline) mods_ |= kMultiline;
    if (p_.flags.dotAll) mods_ |= kDotAll;
  }

  bool run() {
    Node* root = parseDisjunction();
    if (!root) return false;
    if (pos_ < s_.size()) return fail("Unmatched ')'");
    for (const std::u32string& name : references_) {
      bool found = std::any_of(namedGroups_.begin(), namedGroups_.end(), [&](const NamedGroup& g) { return g.name == name; });
      if (!found) return fail("Invalid named capture referenced");
    }
    p_.root = root;
    p_.captureCount = captureCount_;
    return true;
  }

  const std::string& error() const { return error_; }

 private:
  // (disjunction, alternative) pairs from the root to a position: MightBothParticipate compares them.
  using AltPath = std::vector<std::pair<uint32_t, uint32_t>>;
  struct NamedGroup {
    std::u32string name;
    AltPath path;
  };

  char32_t peek(size_t ahead = 0) const { return pos_ + ahead < s_.size() ? s_[pos_ + ahead] : kEnd; }
  bool atEnd() const { return pos_ >= s_.size(); }

  // Records the first error; returns false, or nullptr through the overloads below.
  bool fail(const char* message) {
    if (error_.empty()) error_ = message;
    return false;
  }
  std::nullptr_t failNode(const char* message) {
    fail(message);
    return nullptr;
  }

  Node* make(Node::Kind kind) {
    Node& n = p_.nodes.emplace_back();
    n.kind = kind;
    n.mods = mods_;
    return &n;
  }
  Node* makeChar(char32_t c) {
    Node* n = make(Node::Kind::Char);
    n->value = c;
    return n;
  }
  Node* makeClass(const ClassNode* cls) {
    Node* n = make(Node::Kind::Class);
    n->cls = cls;
    return n;
  }
  Node* makeClass(ClassItem item) {
    ClassNode& cls = p_.classes.emplace_back();
    cls.mayContainStrings = mayContainStrings(item);
    cls.items.push_back(std::move(item));
    return makeClass(&cls);
  }
  static ClassItem charItem(char32_t c) {
    ClassItem item;
    item.kind = ClassItem::Kind::Char;
    item.lo = item.hi = c;
    return item;
  }
  static ClassItem escapeItem(char32_t letter) {
    ClassItem item;
    item.kind = ClassItem::Kind::Escape;
    item.escape = static_cast<char>(letter);
    return item;
  }

  static bool mayContainStrings(const ClassItem& item) {
    switch (item.kind) {
      case ClassItem::Kind::Property:
        return item.property.kind == unicode::Property::Kind::Strings && !item.negated;
      case ClassItem::Kind::Nested:
        return !item.nested->negated && item.nested->mayContainStrings;
      case ClassItem::Kind::Strings:
        return std::any_of(item.strings.begin(), item.strings.end(), [](const std::u32string& s) { return s.size() != 1; });
      default:
        return false;
    }
  }

  // ---- Disjunction, Alternative, Term ----

  // The Pattern's Disjunction. Groups nest through an explicit stack of frames, not recursion, so the C stack
  // doesn't grow with the nesting.
  Node* parseDisjunction() {
    std::vector<GroupFrame> stack(1);
    stack.back().sequence = make(Node::Kind::Sequence);
    altPath_.emplace_back(disjunctions_++, 0);
    for (;;) {
      if (atEnd() || peek() == ')') {
        if (atEnd() && stack.size() > 1) return failNode("Unterminated group");
        if (!atEnd() && stack.size() == 1) return failNode("Unmatched ')'");
        GroupFrame& top = stack.back();
        top.alternatives.push_back(top.sequence);
        Node* body = top.alternatives[0];
        if (top.alternatives.size() > 1) {
          body = make(Node::Kind::Disjunction);
          body->children = std::move(top.alternatives);
        }
        altPath_.pop_back();
        if (stack.size() == 1) return body;
        pos_++;  // the )
        GroupFrame done = std::move(stack.back());
        stack.pop_back();
        mods_ = done.savedMods;
        done.group->children.push_back(body);
        done.group->lastCapture = captureCount_ + 1;
        Node* term = parseQuantifier(done.group, done.quantifiable, done.capturesBefore);
        if (!term) return nullptr;
        stack.back().sequence->children.push_back(term);
      } else if (peek() == '|') {
        pos_++;
        GroupFrame& top = stack.back();
        top.alternatives.push_back(top.sequence);
        top.sequence = make(Node::Kind::Sequence);
        altPath_.back().second++;
      } else if (peek() == '(') {
        GroupFrame f;
        if (!openGroup(&f)) return nullptr;
        stack.push_back(std::move(f));
        altPath_.emplace_back(disjunctions_++, 0);
      } else {
        Node* term = parseTerm();
        if (!term) return nullptr;
        stack.back().sequence->children.push_back(term);
      }
    }
  }

  Node* parseTerm() {
    uint32_t capturesBefore = captureCount_;
    bool quantifiable = true;
    Node* atom = nullptr;
    char32_t c = peek();
    switch (c) {
      case '^':
      case '$':
        pos_++;
        atom = make(c == '^' ? Node::Kind::LineStart : Node::Kind::LineEnd);
        quantifiable = false;
        break;
      case '\\':
        if (peek(1) == 'b' || peek(1) == 'B') {
          atom = make(peek(1) == 'b' ? Node::Kind::WordBoundary : Node::Kind::NotWordBoundary);
          pos_ += 2;
          quantifiable = false;
        } else {
          atom = parseAtomEscape();
        }
        break;
      case '[':
        atom = parseClass();
        break;
      case '.':
        pos_++;
        atom = make(Node::Kind::Dot);
        break;
      case '*':
      case '+':
      case '?':
        return failNode("Nothing to repeat");
      case '{':
        if (u_) return failNode("Lone quantifier brackets");
        // Annex B: ExtendedAtom :: InvalidBracedQuantifier is an early error; any other { is a character.
        if (scanBraced().ok) return failNode("Nothing to repeat");
        pos_++;
        atom = makeChar(c);
        break;
      case '}':
      case ']':
        if (u_) return failNode("Lone quantifier brackets");
        pos_++;
        atom = makeChar(c);
        break;
      default:
        pos_++;
        atom = makeChar(c);
        break;
    }
    if (!atom) return nullptr;
    return parseQuantifier(atom, quantifiable, capturesBefore);
  }

  struct Braced {
    bool ok = false;
    bool outOfOrder = false;
    uint32_t min = 0;
    uint32_t max = 0;
    size_t end = 0;
  };
  Digits scanDigits(size_t& i) const {
    Digits d;
    size_t start = i;
    uint64_t value = 0;
    while (i < s_.size() && isDecimalDigit(s_[i])) {
      value = std::min<uint64_t>(value * 10 + (s_[i] - '0'), UINT32_MAX);
      i++;
    }
    size_t firstSignificant = start;
    while (firstSignificant + 1 < i && s_[firstSignificant] == '0') firstSignificant++;
    d.value = static_cast<uint32_t>(value);
    d.text = std::u32string_view(s_).substr(firstSignificant, i - firstSignificant);
    return d;
  }
  // `{` DecimalDigits (`,` DecimalDigits?)? `}` at pos_ (not consumed).
  Braced scanBraced() const {
    Braced b;
    size_t i = pos_ + 1;
    if (i >= s_.size() || !isDecimalDigit(s_[i])) return b;
    Digits lo = scanDigits(i);
    Digits hi = lo;
    bool unbounded = false;
    if (i < s_.size() && s_[i] == ',') {
      i++;
      if (i < s_.size() && isDecimalDigit(s_[i])) {
        hi = scanDigits(i);
      } else {
        unbounded = true;
      }
    }
    if (i >= s_.size() || s_[i] != '}') return b;
    b.ok = true;
    b.min = lo.value;
    b.max = unbounded ? Node::kInfinity : hi.value;
    b.outOfOrder = !unbounded && greaterThan(lo, hi);
    b.end = i + 1;
    return b;
  }

  Node* parseQuantifier(Node* atom, bool quantifiable, uint32_t capturesBefore) {
    uint32_t min;
    uint32_t max;
    switch (peek()) {
      case '*':
        min = 0, max = Node::kInfinity;
        pos_++;
        break;
      case '+':
        min = 1, max = Node::kInfinity;
        pos_++;
        break;
      case '?':
        min = 0, max = 1;
        pos_++;
        break;
      case '{': {
        Braced b = scanBraced();
        if (!b.ok) {
          if (u_) return failNode("Incomplete quantifier");
          return atom;  // Annex B: the { is a character
        }
        if (!quantifiable) return failNode("Nothing to repeat");
        if (b.outOfOrder) return failNode("numbers out of order in {} quantifier");
        min = b.min, max = b.max;
        pos_ = b.end;
        break;
      }
      default:
        return atom;
    }
    if (!quantifiable) return failNode("Nothing to repeat");
    Node* q = make(Node::Kind::Quantifier);
    q->min = min;
    q->max = max;
    if (peek() == '?') {
      pos_++;
      q->greedy = false;
    }
    q->firstCapture = capturesBefore + 1;
    q->lastCapture = captureCount_ + 1;
    q->children.push_back(atom);
    return q;
  }

  // ---- groups ----

  // A group being parsed: the main loop in parseDisjunction keeps a stack of them instead of recursing.
  struct GroupFrame {
    Node* group = nullptr;  // nullptr: the Pattern itself
    bool quantifiable = true;
    uint32_t capturesBefore = 0;  // for the quantifier's ClearCaptures range, which includes the group
    uint8_t savedMods = 0;        // restored at the )
    std::vector<Node*> alternatives;
    Node* sequence = nullptr;
  };

  // The opener of a group at the `(`, through `(`, `(?:`, `(?=`, `(?<=`, `(?<name>` or `(?ims-ims:`.
  bool openGroup(GroupFrame* f) {
    f->capturesBefore = captureCount_;
    f->savedMods = mods_;
    Node* n;
    if (peek(1) == '?') {
      char32_t c2 = peek(2);
      if (c2 == '=' || c2 == '!') {
        pos_ += 3;
        n = make(Node::Kind::Lookahead);
        n->negate = c2 == '!';
        f->quantifiable = !u_;  // Annex B: QuantifiableAssertion
      } else if (c2 == '<' && (peek(3) == '=' || peek(3) == '!')) {
        n = make(Node::Kind::Lookbehind);
        n->negate = peek(3) == '!';
        pos_ += 4;
        f->quantifiable = false;
      } else if (c2 == '<') {
        pos_ += 3;
        std::u32string name;
        if (!parseGroupName(&name)) return false;
        for (const NamedGroup& g : namedGroups_) {
          if (g.name == name && mightBothParticipate(g.path, altPath_)) return fail("Duplicate capture group name");
        }
        namedGroups_.push_back({name, altPath_});
        n = make(Node::Kind::Group);
        n->index = ++captureCount_;
        n->name = toUtf16(name);
        p_.groupNames.push_back(n->name);
      } else {
        pos_ += 2;
        if (!parseModifiers()) return false;
        n = make(Node::Kind::NonCapturing);
      }
    } else {
      pos_++;
      n = make(Node::Kind::Group);
      n->index = ++captureCount_;
      p_.groupNames.emplace_back();
    }
    n->firstCapture = captureCount_ + 1;
    f->group = n;
    f->sequence = make(Node::Kind::Sequence);
    return true;
  }

  // RegularExpressionModifiers (- RegularExpressionModifiers)? `:` after the `(?`, applied to mods_ for the body;
  // `(?:` is the case with none.
  bool parseModifiers() {
    uint8_t add = 0;
    uint8_t remove = 0;
    bool dash = false;
    for (;;) {
      char32_t c = peek();
      uint8_t bit = c == 'i' ? kIgnoreCase : c == 'm' ? kMultiline : c == 's' ? kDotAll : 0;
      if (bit) {
        uint8_t& set = dash ? remove : add;
        if (set & bit) return fail("Repeated flag in modifiers");
        set |= bit;
      } else if (c == '-' && !dash) {
        dash = true;
      } else if (c == ':') {
        pos_++;
        break;
      } else {
        return fail("Invalid group");
      }
      pos_++;
    }
    if (dash && !add && !remove) return fail("Invalid group");
    if (add & remove) return fail("Repeated flag in modifiers");
    mods_ = static_cast<uint8_t>((mods_ | add) & ~remove);
    return true;
  }

  static bool mightBothParticipate(const AltPath& a, const AltPath& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
      if (a[i] != b[i]) return !(a[i].first == b[i].first && a[i].second != b[i].second);
    }
    return true;
  }

  // GroupName after the `<`, through the `>`: RegExpIdentifierName with its early errors.
  bool parseGroupName(std::u32string* name) {
    for (;;) {
      if (atEnd()) return fail("Invalid capture group name");
      char32_t c = peek();
      if (c == '>') {
        pos_++;
        break;
      }
      char32_t cp;
      if (c == '\\') {
        pos_++;
        if (peek() != 'u') return fail("Invalid capture group name");
        pos_++;
        auto escaped = parseUnicodeEscapeBody(true);
        if (!escaped) return fail("Invalid Unicode escape in capture group name");
        cp = *escaped;
      } else if (!u_ && isLead(c) && isTrail(peek(1))) {
        cp = combineSurrogates(c, peek(1));
        pos_ += 2;
      } else {
        cp = c;
        pos_++;
      }
      bool ok = name->empty() ? cp == '$' || cp == '_' || unicode::isIDStart(cp)
                              : cp == '$' || cp == 0x200C || cp == 0x200D || unicode::isIDContinue(cp);
      if (!ok) return fail("Invalid capture group name");
      *name += cp;
    }
    if (name->empty()) return fail("Invalid capture group name");
    return true;
  }

  // ---- escapes ----

  // RegExpUnicodeEscapeSequence after the `u`; nullopt (pos_ unchanged) if it doesn't match.
  std::optional<char32_t> parseUnicodeEscapeBody(bool unicodeMode) {
    if (unicodeMode && peek() == '{') {
      size_t i = pos_ + 1;
      uint32_t value = 0;
      while (i < s_.size() && isHexDigit(s_[i])) {
        value = value * 16 + hexValue(s_[i]);
        if (value > 0x10FFFF) return std::nullopt;
        i++;
      }
      if (i == pos_ + 1 || i >= s_.size() || s_[i] != '}') return std::nullopt;
      pos_ = i + 1;
      return value;
    }
    auto hex4 = [&](size_t at) -> std::optional<char32_t> {
      if (at + 4 > s_.size()) return std::nullopt;
      char32_t value = 0;
      for (size_t k = at; k < at + 4; k++) {
        if (!isHexDigit(s_[k])) return std::nullopt;
        value = value * 16 + hexValue(s_[k]);
      }
      return value;
    };
    auto value = hex4(pos_);
    if (!value) return std::nullopt;
    pos_ += 4;
    if (unicodeMode && isLead(*value) && peek() == '\\' && peek(1) == 'u') {
      auto trail = hex4(pos_ + 2);
      if (trail && isTrail(*trail)) {
        pos_ += 6;
        return combineSurrogates(*value, *trail);
      }
    }
    return value;
  }

  // LegacyOctalEscapeSequence at pos_ (an octal digit).
  char32_t parseLegacyOctal() {
    char32_t d0 = peek() - '0';
    pos_++;
    if (!isOctalDigit(peek())) return d0;
    char32_t d1 = peek() - '0';
    pos_++;
    if (d0 <= 3 && isOctalDigit(peek())) {
      char32_t d2 = peek() - '0';
      pos_++;
      return d0 * 64 + d1 * 8 + d2;
    }
    return d0 * 8 + d1;
  }

  // CharacterEscape after the `\`, for the cases the callers don't handle themselves. Without u, `\c` not
  // followed by a control letter is the `\` alone (pos_ stays at the `c`).
  std::optional<char32_t> parseCharacterEscape(bool inClass) {
    char32_t c = peek();
    switch (c) {
      case 'f':
        pos_++;
        return 0x0C;
      case 'n':
        pos_++;
        return 0x0A;
      case 'r':
        pos_++;
        return 0x0D;
      case 't':
        pos_++;
        return 0x09;
      case 'v':
        pos_++;
        return 0x0B;
      case 'c': {
        char32_t letter = peek(1);
        if (isAsciiLetter(letter) || (!u_ && inClass && (isDecimalDigit(letter) || letter == '_'))) {
          pos_ += 2;
          return letter % 32;
        }
        if (u_) return fail("Invalid unicode escape"), std::nullopt;
        return U'\\';
      }
      case 'x':
        if (isHexDigit(peek(1)) && isHexDigit(peek(2))) {
          char32_t value = hexValue(peek(1)) * 16 + hexValue(peek(2));
          pos_ += 3;
          return value;
        }
        if (u_) return fail("Invalid escape"), std::nullopt;
        pos_++;
        return c;
      case 'u': {
        pos_++;
        if (auto value = parseUnicodeEscapeBody(u_)) return value;
        if (u_) return fail("Invalid Unicode escape"), std::nullopt;
        return c;
      }
      case 'k':
        if (named_) return fail("Invalid named reference"), std::nullopt;
        break;
    }
    if (u_) {
      if (!isSyntaxCharacter(c) && c != '/' && !(inClass && c == '-')) return fail("Invalid escape"), std::nullopt;
    }
    pos_++;
    return c;
  }

  Node* parseAtomEscape() {
    pos_++;  // the backslash
    if (atEnd()) return failNode("\\ at end of pattern");
    char32_t c = peek();
    switch (c) {
      case 'd':
      case 'D':
      case 's':
      case 'S':
      case 'w':
      case 'W':
        pos_++;
        return makeClass(escapeItem(c));
      case 'p':
      case 'P':
        if (u_) {
          pos_++;
          ClassItem item;
          if (!parsePropertyEscape(&item, c == 'P')) return nullptr;
          return makeClass(std::move(item));
        }
        break;
      case 'k':
        if (named_) {
          pos_++;
          if (peek() != '<') return failNode("Invalid named reference");
          pos_++;
          std::u32string name;
          if (!parseGroupName(&name)) return nullptr;
          references_.push_back(name);
          Node* n = make(Node::Kind::NamedBackreference);
          n->name = toUtf16(name);
          return n;
        }
        break;
      case '0':
        if (!isDecimalDigit(peek(1))) {
          pos_++;
          return makeChar(0);
        }
        if (u_) return failNode("Invalid decimal escape");
        return makeChar(parseLegacyOctal());
      default:
        if (isDecimalDigit(c)) {
          size_t start = pos_;
          Digits n = scanDigits(pos_);
          if (n.value <= totalCaptures_) {
            Node* ref = make(Node::Kind::Backreference);
            ref->index = n.value;
            return ref;
          }
          if (u_) return failNode("Invalid escape");
          // Annex B: not a DecimalEscape; a legacy octal escape, or \8 and \9 as identity escapes.
          pos_ = start;
          if (isOctalDigit(c)) return makeChar(parseLegacyOctal());
          pos_++;
          return makeChar(c);
        }
        break;
    }
    auto ch = parseCharacterEscape(false);
    if (!ch) return nullptr;
    return makeChar(*ch);
  }

  // \p{...} / \P{...} after the p.
  [[gnu::noinline]] bool parsePropertyEscape(ClassItem* item, bool negated) {
    if (peek() != '{') return fail("Invalid property name");
    pos_++;
    std::string name;
    std::string value;
    bool hasValue = false;
    for (;;) {
      if (atEnd()) return fail("Invalid property name");
      char32_t c = peek();
      pos_++;
      if (c == '}') break;
      if (c == '=' && !hasValue) {
        hasValue = true;
        continue;
      }
      if (!isAsciiLetter(c) && !isDecimalDigit(c) && c != '_') return fail("Invalid property name");
      (hasValue ? value : name) += static_cast<char>(c);
    }
    std::optional<unicode::Property> property;
    if (hasValue) {
      bool nameOk = !name.empty() && std::none_of(name.begin(), name.end(), [](char ch) { return isDecimalDigit(ch); });
      if (nameOk && !value.empty()) property = unicode::lookupProperty(name, value);
    } else if (!name.empty()) {
      property = unicode::lookupLoneProperty(name);
      if (property && property->kind == unicode::Property::Kind::Strings && (!v_ || negated)) property.reset();
    }
    if (!property) return fail("Invalid property name");
    item->kind = ClassItem::Kind::Property;
    item->property = *property;
    item->negated = negated;
    item->loneGeneralCategory = !hasValue && property->kind == unicode::Property::Kind::GeneralCategory;
    return true;
  }

  // ---- character classes ----

  Node* parseClass() {
    if (v_) {
      const ClassNode* cls = parseClassSetClass();
      return cls ? makeClass(cls) : nullptr;
    }
    pos_++;
    ClassNode& cls = p_.classes.emplace_back();
    if (peek() == '^') {
      pos_++;
      cls.negated = true;
    }
    for (;;) {
      if (atEnd()) return failNode("Unterminated character class");
      if (peek() == ']') {
        pos_++;
        break;
      }
      ClassItem a;
      if (!parseClassAtom(&a)) return nullptr;
      if (peek() == '-' && peek(1) != ']' && peek(1) != kEnd) {
        pos_++;
        ClassItem b;
        if (!parseClassAtom(&b)) return nullptr;
        if (a.kind != ClassItem::Kind::Char || b.kind != ClassItem::Kind::Char) {
          if (u_) return failNode("Invalid character class");
          // Annex B CharacterRangeOrUnion: the two sets and a -.
          cls.items.push_back(std::move(a));
          cls.items.push_back(std::move(b));
          cls.items.push_back(charItem('-'));
          continue;
        }
        if (a.lo > b.lo) return failNode("Range out of order in character class");
        a.kind = ClassItem::Kind::Range;
        a.hi = b.lo;
      }
      cls.items.push_back(std::move(a));
    }
    return makeClass(&cls);
  }

  // ClassAtom (no v flag).
  bool parseClassAtom(ClassItem* item) {
    char32_t c = peek();
    if (c != '\\') {
      pos_++;
      *item = charItem(c);
      return true;
    }
    pos_++;
    if (atEnd()) return fail("\\ at end of pattern");
    c = peek();
    switch (c) {
      case 'b':
        pos_++;
        *item = charItem(0x08);
        return true;
      case 'd':
      case 'D':
      case 's':
      case 'S':
      case 'w':
      case 'W':
        pos_++;
        *item = escapeItem(c);
        return true;
      case 'p':
      case 'P':
        if (u_) {
          pos_++;
          return parsePropertyEscape(item, c == 'P');
        }
        break;
      case '0':
        if (!isDecimalDigit(peek(1))) {
          pos_++;
          *item = charItem(0);
          return true;
        }
        if (u_) return fail("Invalid class escape");
        *item = charItem(parseLegacyOctal());
        return true;
      default:
        if (isDecimalDigit(c)) {
          if (u_) return fail("Invalid class escape");
          if (isOctalDigit(c)) {
            *item = charItem(parseLegacyOctal());
          } else {
            pos_++;
            *item = charItem(c);
          }
          return true;
        }
        break;
    }
    auto ch = parseCharacterEscape(true);
    if (!ch) return false;
    *item = charItem(*ch);
    return true;
  }

  // CharacterClass / NestedClass with the v flag, at the `[`.
  const ClassNode* parseClassSetClass() {
    if (++depth_ > kMaxClassDepth) return failNode("Regular expression too large");
    pos_++;
    ClassNode& cls = p_.classes.emplace_back();
    if (peek() == '^') {
      pos_++;
      cls.negated = true;
    }
    if (!parseClassSetExpression(&cls)) return nullptr;
    if (cls.negated && cls.mayContainStrings) return failNode("Negated character class may contain strings");
    depth_--;
    return &cls;
  }

  bool atDoubleAmpersand() const { return peek() == '&' && peek(1) == '&'; }
  bool atDoubleDash() const { return peek() == '-' && peek(1) == '-'; }

  // ClassSetExpression through the closing `]`.
  bool parseClassSetExpression(ClassNode* cls) {
    if (atEnd()) return fail("Unterminated character class");
    if (peek() == ']') {
      pos_++;
      return true;
    }
    ClassItem first;
    bool firstIsRange = false;
    if (!parseClassSetOperandOrRange(&first, &firstIsRange)) return false;
    cls->items.push_back(std::move(first));
    if (atDoubleAmpersand() || atDoubleDash()) {
      if (firstIsRange) return fail("Invalid set operation in character class");
      bool intersection = atDoubleAmpersand();
      cls->op = intersection ? ClassNode::Op::Intersection : ClassNode::Op::Subtraction;
      while (intersection ? atDoubleAmpersand() : atDoubleDash()) {
        pos_ += 2;
        if (intersection && peek() == '&') return fail("Invalid character in character class");
        ClassItem operand;
        bool isChar;
        if (!parseClassSetOperand(&operand, &isChar)) return false;
        cls->items.push_back(std::move(operand));
      }
      if (atEnd()) return fail("Unterminated character class");
      if (peek() != ']') return fail("Invalid set operation in character class");
      pos_++;
      if (intersection) {
        cls->mayContainStrings = std::all_of(cls->items.begin(), cls->items.end(), [](const ClassItem& i) { return mayContainStrings(i); });
      } else {
        cls->mayContainStrings = mayContainStrings(cls->items[0]);
      }
      return true;
    }
    for (;;) {
      if (atEnd()) return fail("Unterminated character class");
      if (peek() == ']') {
        pos_++;
        break;
      }
      if (atDoubleAmpersand() || atDoubleDash()) return fail("Invalid set operation in character class");
      ClassItem item;
      bool isRange;
      if (!parseClassSetOperandOrRange(&item, &isRange)) return false;
      cls->items.push_back(std::move(item));
    }
    cls->mayContainStrings = std::any_of(cls->items.begin(), cls->items.end(), [](const ClassItem& i) { return mayContainStrings(i); });
    return true;
  }

  // ClassSetRange or ClassSetOperand.
  bool parseClassSetOperandOrRange(ClassItem* item, bool* isRange) {
    bool isChar;
    *isRange = false;
    if (!parseClassSetOperand(item, &isChar)) return false;
    if (isChar && peek() == '-' && peek(1) != '-') {
      pos_++;
      ClassItem last;
      bool lastIsChar;
      if (!parseClassSetOperand(&last, &lastIsChar)) return false;
      if (!lastIsChar) return fail("Invalid character class");
      if (item->lo > last.lo) return fail("Range out of order in character class");
      item->kind = ClassItem::Kind::Range;
      item->hi = last.lo;
      *isRange = true;
    }
    return true;
  }

  bool parseClassSetOperand(ClassItem* item, bool* isChar) {
    *isChar = false;
    if (atEnd()) return fail("Unterminated character class");
    char32_t c = peek();
    if (c == '[') {
      const ClassNode* nested = parseClassSetClass();
      if (!nested) return false;
      item->kind = ClassItem::Kind::Nested;
      item->nested = nested;
      return true;
    }
    if (c == '\\') {
      char32_t c1 = peek(1);
      switch (c1) {
        case 'q':
          if (peek(2) != '{') return fail("Invalid escape");
          pos_ += 3;
          return parseClassStringDisjunction(item);
        case 'd':
        case 'D':
        case 's':
        case 'S':
        case 'w':
        case 'W':
          pos_ += 2;
          *item = escapeItem(c1);
          return true;
        case 'p':
        case 'P':
          pos_ += 2;
          return parsePropertyEscape(item, c1 == 'P');
      }
    }
    auto ch = parseClassSetCharacter();
    if (!ch) return false;
    *item = charItem(*ch);
    *isChar = true;
    return true;
  }

  [[gnu::noinline]] std::optional<char32_t> parseClassSetCharacter() {
    char32_t c = peek();
    if (c == '\\') {
      pos_++;
      if (atEnd()) return fail("\\ at end of pattern"), std::nullopt;
      c = peek();
      if (c == 'b') {
        pos_++;
        return 0x08;
      }
      if (isClassSetReservedPunctuator(c)) {
        pos_++;
        return c;
      }
      if (c == '0') {
        if (isDecimalDigit(peek(1))) return fail("Invalid class escape"), std::nullopt;
        pos_++;
        return 0;
      }
      if (isDecimalDigit(c)) return fail("Invalid class escape"), std::nullopt;
      return parseCharacterEscape(true);
    }
    if (isClassSetSyntaxCharacter(c)) return fail("Invalid character in character class"), std::nullopt;
    if (isClassSetDoublePunctuatorChar(c) && peek(1) == c) return fail("Invalid set operation in character class"), std::nullopt;
    pos_++;
    return c;
  }

  // \q{...} after the `{`.
  [[gnu::noinline]] bool parseClassStringDisjunction(ClassItem* item) {
    item->kind = ClassItem::Kind::Strings;
    std::u32string current;
    for (;;) {
      if (atEnd()) return fail("Unterminated character class");
      char32_t c = peek();
      if (c == '}' || c == '|') {
        pos_++;
        item->strings.push_back(std::move(current));
        current.clear();
        if (c == '}') return true;
        continue;
      }
      auto ch = parseClassSetCharacter();
      if (!ch) return false;
      current += *ch;
    }
  }

  Pattern& p_;
  const std::u32string s_;
  size_t pos_ = 0;
  const bool u_;      // UnicodeMode
  const bool v_;      // UnicodeSetsMode
  const bool named_;  // NamedCaptureGroups
  const uint32_t totalCaptures_;
  uint8_t mods_ = 0;
  int depth_ = 0;
  uint32_t captureCount_ = 0;
  uint32_t disjunctions_ = 0;
  AltPath altPath_;
  std::vector<NamedGroup> namedGroups_;
  std::vector<std::u32string> references_;
  std::string error_;
};

std::string flagsText(const Flags& f) {
  std::string out;
  if (f.hasIndices) out += 'd';
  if (f.global) out += 'g';
  if (f.ignoreCase) out += 'i';
  if (f.multiline) out += 'm';
  if (f.dotAll) out += 's';
  if (f.unicode) out += 'u';
  if (f.unicodeSets) out += 'v';
  if (f.sticky) out += 'y';
  return out;
}

}  // namespace

std::optional<Flags> parseFlags(std::u16string_view text) {
  Flags f;
  for (char16_t c : text) {
    bool* flag;
    switch (c) {
      case 'd':
        flag = &f.hasIndices;
        break;
      case 'g':
        flag = &f.global;
        break;
      case 'i':
        flag = &f.ignoreCase;
        break;
      case 'm':
        flag = &f.multiline;
        break;
      case 's':
        flag = &f.dotAll;
        break;
      case 'u':
        flag = &f.unicode;
        break;
      case 'v':
        flag = &f.unicodeSets;
        break;
      case 'y':
        flag = &f.sticky;
        break;
      default:
        return std::nullopt;
    }
    if (*flag) return std::nullopt;
    *flag = true;
  }
  if (f.unicode && f.unicodeSets) return std::nullopt;
  return f;
}

std::unique_ptr<Pattern> parsePattern(std::u16string_view source, const Flags& flags, std::string* error) {
  bool unicodeMode = flags.unicode || flags.unicodeSets;
  std::u32string s = decodePattern(source, unicodeMode);
  CaptureScan scan = scanCaptures(s, flags.unicodeSets);
  auto pattern = std::make_unique<Pattern>();
  pattern->flags = flags;
  // Annex B ParsePattern: without u or v, [+NamedCaptureGroups] only if the pattern has a GroupName.
  Parser parser(*pattern, std::move(s), unicodeMode || scan.hasNames, scan.count);
  if (!parser.run()) {
    *error = "Invalid regular expression: /" + toUtf8(source) + "/" + flagsText(flags) + ": " + parser.error();
    return nullptr;
  }
  return pattern;
}

}  // namespace snowman::regexp
