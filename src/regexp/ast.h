// The parsed form of a RegExp pattern (internal to src/regexp). The parser (parser.cpp) builds it from the
// pattern text per ECMA-262 2025 22.2.1 and Annex B.1.2, with every early error checked; the compiler lowers it
// to the matcher's program.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "regexp/regexp.h"
#include "unicode/unicode.h"

namespace snowman::regexp {

// Flags a modifier group (?ims-ims:...) can change; each node records the ones in effect where it appears.
enum ModFlag : uint8_t { kIgnoreCase = 1, kMultiline = 2, kDotAll = 4 };

struct ClassNode;

// One operand of a character class: in a NonemptyClassRanges (no v flag) every item is unioned; with v, items are
// the operands of op.
struct ClassItem {
  enum class Kind : uint8_t {
    Char,      // lo
    Range,     // lo..hi
    Escape,    // \d \D \s \S \w \W: escape is the letter
    Property,  // \p{...} (negated: \P{...})
    Nested,    // [...] inside a v-mode class
    Strings,   // \q{...}
  };
  Kind kind = Kind::Char;
  char escape = 0;
  bool negated = false;
  char32_t lo = 0;
  char32_t hi = 0;
  unicode::Property property{};
  bool loneGeneralCategory = false;  // \p{Lu} rather than \p{gc=Lu}: CompileToCharSet doesn't case-fold it
  const ClassNode* nested = nullptr;
  std::vector<std::u32string> strings;
};

struct ClassNode {
  enum class Op : uint8_t { Union, Intersection, Subtraction };
  Op op = Op::Union;
  bool negated = false;
  bool mayContainStrings = false;  // MayContainStrings (v flag)
  std::vector<ClassItem> items;
};

struct Node {
  enum class Kind : uint8_t {
    Empty,
    Char,             // value: a code point with u/v, else a code unit
    Dot,              // .
    Class,            // cls: a CharacterClass or a class escape outside a class
    Sequence,         // children, in order
    Disjunction,      // children are the alternatives
    Group,            // capturing: index (1-based), children[0]
    NonCapturing,     // (?:...) and modifier groups: children[0]
    Lookahead,        // negate, children[0]
    Lookbehind,       // negate, children[0]
    Quantifier,       // min, max (kInfinity), greedy, children[0]; firstCapture..lastCapture are reset per iteration
    Backreference,    // index
    NamedBackreference,  // name: every group with that name, see Pattern::groupNames
    LineStart,        // ^
    LineEnd,          // $
    WordBoundary,     // \b
    NotWordBoundary,  // \B
  };
  static constexpr uint32_t kInfinity = UINT32_MAX;

  Kind kind = Kind::Empty;
  uint8_t mods = 0;  // ModFlag bits in effect
  bool negate = false;
  bool greedy = true;
  char32_t value = 0;
  uint32_t index = 0;
  uint32_t min = 0;
  uint32_t max = 0;
  uint32_t firstCapture = 0;  // Quantifier, groups, lookarounds: the captures inside are [firstCapture, lastCapture)
  uint32_t lastCapture = 0;
  const ClassNode* cls = nullptr;
  std::u16string name;
  std::vector<Node*> children;
};

struct Pattern {
  Flags flags;
  Node* root = nullptr;
  uint32_t captureCount = 0;
  // groupNames[i] is the name of capture group i + 1, empty if it has none (in left-paren order).
  std::vector<std::u16string> groupNames;

  // Arena: nodes live as long as the Pattern.
  std::deque<Node> nodes;
  std::deque<ClassNode> classes;
};

// The flags of a RegExp literal or of the RegExp constructor; nullopt if any is unknown or repeated, or if u
// and v are both present.
std::optional<Flags> parseFlags(std::u16string_view flags);

// Parses `source` (UTF-16 code units). On a SyntaxError returns nullptr and sets *error to the message.
std::unique_ptr<Pattern> parsePattern(std::u16string_view source, const Flags& flags, std::string* error);

}  // namespace snowman::regexp
