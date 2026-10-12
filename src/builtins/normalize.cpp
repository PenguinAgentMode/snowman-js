// Unicode normalization: decomposition, canonical ordering and canonical composition, with Hangul handled
// algorithmically (UAX #15, Unicode 3.12).
#include "builtins/normalize.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace snowman::builtins {

namespace {

struct DecompositionEntry {
  char32_t cp;
  uint16_t offset;
  uint8_t length;
};
struct CombiningClassEntry {
  char32_t cp;
  uint8_t rank;
};
struct CompositionEntry {
  char32_t first;
  char32_t second;
  char32_t composite;
};
#include "builtins/normalize_tables.inc"

constexpr char32_t SBase = 0xAC00, LBase = 0x1100, VBase = 0x1161, TBase = 0x11A7;
constexpr int LCount = 19, VCount = 21, TCount = 28, NCount = VCount * TCount, SCount = LCount * NCount;

int combiningClass(char32_t cp) {
  auto it = std::lower_bound(std::begin(kCombiningClass), std::end(kCombiningClass), cp,
                             [](const CombiningClassEntry& e, char32_t c) { return e.cp < c; });
  return it != std::end(kCombiningClass) && it->cp == cp ? it->rank : 0;
}

void decompose(char32_t cp, bool compatibility, std::vector<char32_t>& out) {
  if (cp >= SBase && cp < SBase + SCount) {
    int s = int(cp - SBase);
    out.push_back(LBase + s / NCount);
    out.push_back(VBase + (s % NCount) / TCount);
    if (s % TCount) out.push_back(TBase + s % TCount);
    return;
  }
  const DecompositionEntry* begin = compatibility ? std::begin(kCompatibilityDecomposition) : std::begin(kCanonicalDecomposition);
  const DecompositionEntry* end = compatibility ? std::end(kCompatibilityDecomposition) : std::end(kCanonicalDecomposition);
  const char32_t* pool = compatibility ? kCompatibilityDecompositionPool : kCanonicalDecompositionPool;
  auto it = std::lower_bound(begin, end, cp, [](const DecompositionEntry& e, char32_t c) { return e.cp < c; });
  if (it != end && it->cp == cp) {
    out.insert(out.end(), pool + it->offset, pool + it->offset + it->length);
  } else {
    out.push_back(cp);
  }
}

char32_t compose(char32_t a, char32_t b) {
  if (a >= LBase && a < LBase + LCount && b >= VBase && b < VBase + VCount)
    return SBase + ((a - LBase) * VCount + (b - VBase)) * TCount;
  if (a >= SBase && a < SBase + SCount && (a - SBase) % TCount == 0 && b > TBase && b < TBase + TCount)
    return a + (b - TBase);
  auto it = std::lower_bound(std::begin(kComposition), std::end(kComposition), std::make_pair(a, b),
                             [](const CompositionEntry& e, std::pair<char32_t, char32_t> key) {
                               return e.first < key.first || (e.first == key.first && e.second < key.second);
                             });
  if (it != std::end(kComposition) && it->first == a && it->second == b) return it->composite;
  return 0;
}

}  // namespace

std::u16string Normalize(std::u16string_view input, NormalizationForm form) {
  bool ascii = true;
  for (char16_t c : input) {
    if (c >= 0x80) {
      ascii = false;
      break;
    }
  }
  if (ascii) return std::u16string(input);
  bool compatibility = form == NormalizationForm::NFKC || form == NormalizationForm::NFKD;
  std::vector<char32_t> cps;
  for (size_t i = 0; i < input.size(); i++) {
    char32_t c = input[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < input.size() && input[i + 1] >= 0xDC00 && input[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (input[i + 1] - 0xDC00);
      i++;
    }
    decompose(c, compatibility, cps);
  }
  // Canonical ordering: stable-sort each run of non-starters by combining class.
  for (size_t i = 0; i < cps.size();) {
    if (combiningClass(cps[i]) == 0) {
      i++;
      continue;
    }
    size_t j = i;
    while (j < cps.size() && combiningClass(cps[j]) != 0) j++;
    std::stable_sort(cps.begin() + long(i), cps.begin() + long(j),
                     [](char32_t a, char32_t b) { return combiningClass(a) < combiningClass(b); });
    i = j;
  }
  if (form == NormalizationForm::NFC || form == NormalizationForm::NFKC) {
    if (!cps.empty()) {
      size_t starterPos = 0;
      int lastClass = combiningClass(cps[0]);
      if (lastClass != 0) lastClass = 256;
      size_t compPos = 1;
      for (size_t decompPos = 1; decompPos < cps.size(); decompPos++) {
        char32_t ch = cps[decompPos];
        int chClass = combiningClass(ch);
        char32_t composite = lastClass == 256 ? 0 : compose(cps[starterPos], ch);
        if (composite && (lastClass < chClass || lastClass == 0)) {
          cps[starterPos] = composite;
        } else {
          if (chClass == 0) starterPos = compPos;
          lastClass = chClass;
          cps[compPos++] = ch;
        }
      }
      cps.resize(compPos);
    }
  }
  std::u16string out;
  out.reserve(cps.size());
  for (char32_t c : cps) {
    if (c >= 0x10000) {
      c -= 0x10000;
      out.push_back(char16_t(0xD800 + (c >> 10)));
      out.push_back(char16_t(0xDC00 + (c & 0x3FF)));
    } else {
      out.push_back(char16_t(c));
    }
  }
  return out;
}

}  // namespace snowman::builtins
