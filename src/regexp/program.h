// The compiled form of a pattern (internal to src/regexp): bytecode for the backtracking VM in matcher.cpp,
// produced by compiler.cpp from the AST.
//
// The VM keeps a position, the captures and a register file, and one explicit backtrack stack holding choice
// points and undo records; it never recurses. Character matchers read forward or, inside a lookbehind, backward
// (kBackward), one character at a time: a code point with u or v, else a code unit.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "regexp/charset.h"
#include "regexp/regexp.h"

namespace snowman::regexp {

enum class Op : uint8_t {
  // One character: Char (a = the character), Set (a = index into sets), Dot (anything but a line
  // terminator), Any.
  Char,
  Set,
  Dot,
  Any,
  // Assertions. LineStart/LineEnd: kMultiline; word boundaries: kUnicodeWords for the u/v + i word set.
  LineStart,
  LineEnd,
  WordBoundary,
  NotWordBoundary,
  // BackreferenceMatcher over the groups backrefGroups[a, a + b); kIgnoreCase, kBackward.
  Backref,
  Split,  // push a choice point resuming at a; continue at pc + 1
  Jump,   // continue at a
  // Capturing groups: GroupStart saves the position in register a; GroupEnd sets capture a to the range between
  // register b and the position.
  GroupStart,
  GroupEnd,
  ClearCaptures,  // captures [a, b) to undefined (RepeatMatcher step 4)
  // RepeatMatcher. Registers a (iterations begun) and a + 1 (where the current one began).
  LoopInit,  // register a = 0
  Loop,      // b = min, c = max, d = the exit; kGreedy
  LoopIter,  // begins an iteration: register a + 1, register a + 1 = position
  LoopNext,  // b = min, c = the Loop: the empty check
  // A quantified single-character matcher (the instruction after) with no captures: b = min, c = max; kGreedy.
  // Continues at pc + 2. kFollow: the instruction there is Char d (a BMP non-surrogate, forward), so only
  // positions where the input has d can continue.
  Star,
  // Lookarounds: registers a (backtrack stack height at the start) and a + 1 (the position). LookStart: b = the
  // LookEnd, captures [c, d) are the body's; kNegative.
  LookStart,
  LookEnd,  // b = the LookStart
  Match,
};

enum InstFlag : uint8_t {
  kBackward = 1,
  kGreedy = 2,
  kNegative = 4,
  kInstIgnoreCase = 8,
  kInstMultiline = 16,
  kUnicodeWords = 32,
  kFollow = 64,
};

// A Set's characters, with a bitmap for ASCII.
struct MatchSet {
  explicit MatchSet(CharSet s) : set(std::move(s)) {
    for (const auto& r : set.ranges()) {
      for (char32_t c = r.first; c <= r.last && c < 128; c++) ascii[c >> 6] |= uint64_t{1} << (c & 63);
    }
  }
  bool contains(char32_t c) const { return c < 128 ? (ascii[c >> 6] >> (c & 63)) & 1 : set.contains(c); }
  uint64_t ascii[2] = {0, 0};
  CharSet set;
};

struct Inst {
  Op op;
  uint8_t flags = 0;
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t c = 0;
  uint32_t d = 0;
};

class Program {
 public:
  Flags flags;
  bool unicodeMode = false;  // u or v: characters are code points
  uint32_t captureCount = 0;
  std::vector<std::u16string> groupNames;  // [i] for capture i; [0] unused
  bool hasGroupNames = false;
  // The pattern can only match at index 0 (it starts with ^ and m is off), so the search tries index 0 only.
  bool anchoredStart = false;
  uint32_t registerCount = 0;
  std::vector<Inst> code;
  std::vector<MatchSet> sets;
  // Every match that consumes anything starts with one of these characters (none: no such filter). The search
  // skips other start positions. Only when no match can be empty.
  std::optional<MatchSet> firstChars;
  std::vector<uint32_t> backrefGroups;
};

}  // namespace snowman::regexp
