// CharSet (ECMA-262 22.2.2): sets of characters as sorted, disjoint, non-adjacent inclusive ranges, with the set
// operations the class compiler needs and the case-insensitive closure CharacterSetMatcher relies on.
#pragma once

#include <span>
#include <vector>

#include "unicode/unicode.h"

namespace snowman::regexp {

constexpr char32_t kMaxCodeUnit = 0xFFFF;
constexpr char32_t kMaxCodePoint = 0x10FFFF;

class CharSet {
 public:
  using Range = unicode::CodePointRange;

  CharSet() = default;
  static CharSet of(char32_t c) { return range(c, c); }
  static CharSet range(char32_t lo, char32_t hi);
  static CharSet fromRanges(std::span<const Range> ranges);  // already normalized

  void add(char32_t lo, char32_t hi);  // any order; normalized by the next query
  void add(char32_t c) { add(c, c); }
  void addAll(const CharSet& other);

  bool contains(char32_t c) const;
  bool empty() const;
  const std::vector<Range>& ranges() const;

  CharSet unionWith(const CharSet& other) const;
  CharSet intersect(const CharSet& other) const;
  CharSet subtract(const CharSet& other) const;
  // Every character in [0, max] not in the set.
  CharSet complement(char32_t max) const;

 private:
  void normalize() const;
  mutable std::vector<Range> ranges_;
  mutable bool dirty_ = false;
};

// Canonicalize (22.2.2.7.3) with IgnoreCase: simple case folding with u or v, else the toUppercase rule.
enum class CaseMode : uint8_t { Unicode, Legacy };
char32_t canonicalize(CaseMode mode, char32_t c);
// { Canonicalize(c) : c in a }.
CharSet canonicalizeSet(CaseMode mode, const CharSet& a);
// { c : Canonicalize(c) in Canonicalize(a) }: the characters CharacterSetMatcher(a) accepts with IgnoreCase.
CharSet caseClosure(CaseMode mode, const CharSet& a);
// The code points c with scf(c) = c (AllCharacters with v and i).
const CharSet& simpleCaseFoldFixedPoints();

}  // namespace snowman::regexp
