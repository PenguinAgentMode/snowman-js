#include "regexp/charset.h"

#include <algorithm>

namespace snowman::regexp {

CharSet CharSet::range(char32_t lo, char32_t hi) {
  CharSet s;
  s.ranges_.push_back({lo, hi});
  return s;
}

CharSet CharSet::fromRanges(std::span<const Range> ranges) {
  CharSet s;
  s.ranges_.assign(ranges.begin(), ranges.end());
  return s;
}

void CharSet::add(char32_t lo, char32_t hi) {
  if (!ranges_.empty() && !dirty_ && lo > ranges_.back().last && lo - ranges_.back().last > 1) {
    ranges_.push_back({lo, hi});
    return;
  }
  ranges_.push_back({lo, hi});
  dirty_ = true;
}

void CharSet::addAll(const CharSet& other) {
  for (const Range& r : other.ranges()) add(r.first, r.last);
}

void CharSet::normalize() const {
  if (!dirty_) return;
  std::sort(ranges_.begin(), ranges_.end(), [](const Range& a, const Range& b) { return a.first < b.first; });
  std::vector<Range> out;
  for (const Range& r : ranges_) {
    if (!out.empty() && r.first <= out.back().last + 1) {
      out.back().last = std::max(out.back().last, r.last);
    } else {
      out.push_back(r);
    }
  }
  ranges_ = std::move(out);
  dirty_ = false;
}

const std::vector<CharSet::Range>& CharSet::ranges() const {
  normalize();
  return ranges_;
}

bool CharSet::empty() const { return ranges_.empty(); }

bool CharSet::contains(char32_t c) const {
  const auto& rs = ranges();
  auto it = std::upper_bound(rs.begin(), rs.end(), c, [](char32_t x, const Range& r) { return x < r.first; });
  return it != rs.begin() && c <= std::prev(it)->last;
}

CharSet CharSet::unionWith(const CharSet& other) const {
  CharSet s = *this;
  s.addAll(other);
  s.normalize();
  return s;
}

CharSet CharSet::intersect(const CharSet& other) const {
  const auto& a = ranges();
  const auto& b = other.ranges();
  CharSet s;
  size_t i = 0;
  size_t j = 0;
  while (i < a.size() && j < b.size()) {
    char32_t lo = std::max(a[i].first, b[j].first);
    char32_t hi = std::min(a[i].last, b[j].last);
    if (lo <= hi) s.ranges_.push_back({lo, hi});
    if (a[i].last < b[j].last) {
      i++;
    } else {
      j++;
    }
  }
  return s;
}

CharSet CharSet::complement(char32_t max) const {
  CharSet s;
  char32_t next = 0;
  for (const Range& r : ranges()) {
    if (r.first > max) break;
    if (r.first > next) s.ranges_.push_back({next, r.first - 1});
    next = r.last + 1;
    if (r.last >= max) return s;
  }
  if (next <= max) s.ranges_.push_back({next, max});
  return s;
}

CharSet CharSet::subtract(const CharSet& other) const { return intersect(other.complement(kMaxCodePoint)); }

namespace {

std::span<const unicode::CaseMapping> mappings(CaseMode mode) {
  return mode == CaseMode::Unicode ? unicode::simpleCaseFoldings() : unicode::legacyCanonicalizations();
}

CharSet buildMappedSet(CaseMode mode) {
  CharSet s;
  for (const auto& e : mappings(mode)) s.add(e.from);
  s.ranges();
  return s;
}

// The characters Canonicalize does not map to themselves.
const CharSet& mappedSet(CaseMode mode) {
  static const CharSet unicodeSet = buildMappedSet(CaseMode::Unicode);
  static const CharSet legacySet = buildMappedSet(CaseMode::Legacy);
  return mode == CaseMode::Unicode ? unicodeSet : legacySet;
}

}  // namespace

char32_t canonicalize(CaseMode mode, char32_t c) {
  if (mode == CaseMode::Unicode) return unicode::simpleCaseFold(c);
  return c <= kMaxCodeUnit ? unicode::legacyCanonicalize(static_cast<char16_t>(c)) : c;
}

CharSet canonicalizeSet(CaseMode mode, const CharSet& a) {
  CharSet out = a.subtract(mappedSet(mode));
  const auto& rs = a.ranges();
  size_t i = 0;
  for (const auto& e : mappings(mode)) {
    while (i < rs.size() && rs[i].last < e.from) i++;
    if (i == rs.size()) break;
    if (rs[i].first <= e.from) out.add(e.to);
  }
  out.ranges();
  return out;
}

CharSet caseClosure(CaseMode mode, const CharSet& a) {
  CharSet canon = canonicalizeSet(mode, a);
  CharSet out = canon.subtract(mappedSet(mode));
  for (const auto& e : mappings(mode)) {
    if (canon.contains(e.to)) out.add(e.from);
  }
  out.ranges();
  return out;
}

const CharSet& simpleCaseFoldFixedPoints() {
  static const CharSet fixed = mappedSet(CaseMode::Unicode).complement(kMaxCodePoint);
  return fixed;
}

}  // namespace snowman::regexp
