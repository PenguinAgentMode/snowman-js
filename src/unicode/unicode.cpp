#include "unicode/unicode.h"

#include <algorithm>
#include <iterator>

namespace snowman::unicode {
namespace {

struct Range {
  char32_t first;
  char32_t last;
};

#include "unicode/id_tables.inc"

template <size_t N>
bool inTable(const Range (&table)[N], char32_t cp) {
  auto it = std::upper_bound(std::begin(table), std::end(table), cp, [](char32_t c, const Range& r) { return c < r.first; });
  return it != std::begin(table) && cp <= std::prev(it)->last;
}

}  // namespace

bool isIDStart(char32_t cp) { return inTable(kIDStart, cp); }
bool isIDContinue(char32_t cp) { return inTable(kIDContinue, cp); }
bool isSpaceSeparator(char32_t cp) { return inTable(kSpaceSeparator, cp); }

}  // namespace snowman::unicode
