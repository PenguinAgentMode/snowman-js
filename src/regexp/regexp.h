// The RegExp engine's public seam. Owned by the regexp workstream; signature changes go through the lead.
// Pure C++ over UTF-16 code units: no dependency on the runtime.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowman::regexp {

// Early errors for a RegularExpressionLiteral (and the RegExp constructor): nullopt when `flags` and
// `pattern` are valid per ECMA-262 22.2.1 (Annex B grammar without u/v, the u grammar, the v grammar),
// else a SyntaxError message. Strings are UTF-16 code units, as the lexer cooked them.
std::optional<std::string> validate(std::u16string_view pattern, std::u16string_view flags);

// ---- Matcher ----

struct Flags {
  bool hasIndices = false;   // d
  bool global = false;       // g
  bool ignoreCase = false;   // i
  bool multiline = false;    // m
  bool dotAll = false;       // s
  bool unicode = false;      // u
  bool unicodeSets = false;  // v
  bool sticky = false;       // y
};

// A capture in code units of the input: [start, end), or {-1, -1} for a group that did not participate.
struct Span {
  int32_t start = -1;
  int32_t end = -1;
};

// A compiled pattern ([[RegExpMatcher]] plus the [[RegExpRecord]] facts the built-in needs). Immutable: share it
// between RegExp objects, realms and threads.
class Program;

// The parse in RegExpInitialize (22.2.3.1 steps 10-12): the flags, then the pattern with its early errors, then
// compilation. nullptr with *error set to the SyntaxError message on failure.
std::shared_ptr<const Program> compile(std::u16string_view pattern, std::u16string_view flags, std::string* error);

const Flags& flagsOf(const Program& program);
// [[CapturingGroupsCount]]: the capture groups, not counting the whole match.
uint32_t captureCount(const Program& program);
// The CapturingGroupName of capture i (1 <= i <= captureCount), u"" if it has none. With duplicate named groups
// (ES2025) a name repeats; RegExpBuiltinExec's groups/indices loop resolves them in ascending i.
const std::u16string& groupName(const Program& program, uint32_t i);
// "R contains any GroupName" (whether the match has a groups object).
bool hasGroupNames(const Program& program);

enum class ExecStatus : uint8_t {
  Match,
  NoMatch,
  // The backtrack stack outgrew its limit; the match neither succeeded nor failed. The caller throws (a
  // RangeError, as engines do for a regexp stack overflow).
  ResourceExhausted,
};

// The matching loop of RegExpBuiltinExec (22.2.7.2 steps 12-16) over `input` from `lastIndex` (<= input.size();
// the caller does ToLength, the lastIndex > length check and resets lastIndex without g/y): try at lastIndex
// and, unless the y flag is set, at each AdvanceStringIndex after it until one matches.
// On Match, *spans has captureCount + 1 entries: spans[0] is the spec's Match Record { lastIndex, e } (so with
// u/v and lastIndex inside a surrogate pair, the matcher starts at the pair but spans[0].start is lastIndex,
// as the spec says), spans[i] is capture i, already GetStringIndex'd. The caller sets lastIndex to
// spans[0].end (g/y) and builds the result array, groups and, with d, indices from the spans.
ExecStatus exec(const Program& program, std::u16string_view input, size_t lastIndex, std::vector<Span>* spans);

}  // namespace snowman::regexp
