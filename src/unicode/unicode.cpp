#include "unicode/unicode.h"

#include <algorithm>
#include <iterator>

namespace snowman::unicode {
namespace {

using Range = CodePointRange;

struct RangeRef {
  uint32_t offset;
  uint32_t count;
};
struct NameEntry {
  const char* name;
  uint16_t index;
};
struct StringRef {
  uint32_t offset;
  uint32_t length;
};
struct StringProperty {
  RangeRef codePoints;
  uint32_t firstString;
  uint32_t stringCount;
};

#include "unicode/case_tables.inc"
#include "unicode/id_tables.inc"
#include "unicode/property_tables.inc"

template <size_t N>
bool inTable(const Range (&table)[N], char32_t cp) {
  auto it = std::upper_bound(std::begin(table), std::end(table), cp, [](char32_t c, const Range& r) { return c < r.first; });
  return it != std::begin(table) && cp <= std::prev(it)->last;
}

template <size_t N>
std::optional<uint16_t> findName(const NameEntry (&table)[N], std::string_view name) {
  auto it = std::lower_bound(std::begin(table), std::end(table), name,
                             [](const NameEntry& e, std::string_view n) { return std::string_view(e.name) < n; });
  if (it == std::end(table) || std::string_view(it->name) != name) return std::nullopt;
  return it->index;
}

std::span<const Range> ranges(RangeRef ref) { return {kRanges + ref.offset, ref.count}; }

template <size_t N>
char32_t mapCase(const CaseMapping (&table)[N], char32_t c) {
  auto it = std::lower_bound(std::begin(table), std::end(table), c, [](const CaseMapping& m, char32_t x) { return m.from < x; });
  return it != std::end(table) && it->from == c ? it->to : c;
}

}  // namespace

bool isIDStart(char32_t cp) { return inTable(kIDStart, cp); }
bool isIDContinue(char32_t cp) { return inTable(kIDContinue, cp); }
bool isSpaceSeparator(char32_t cp) { return inTable(kSpaceSeparator, cp); }

std::optional<Property> lookupProperty(std::string_view name, std::string_view value) {
  Property::Kind kind;
  if (name == "General_Category" || name == "gc") {
    kind = Property::Kind::GeneralCategory;
  } else if (name == "Script" || name == "sc") {
    kind = Property::Kind::Script;
  } else if (name == "Script_Extensions" || name == "scx") {
    kind = Property::Kind::ScriptExtensions;
  } else {
    return std::nullopt;
  }
  auto index = kind == Property::Kind::GeneralCategory ? findName(kGeneralCategoryNames, value) : findName(kScriptNames, value);
  if (!index) return std::nullopt;
  return Property{kind, *index};
}

std::optional<Property> lookupLoneProperty(std::string_view nameOrValue) {
  if (auto i = findName(kGeneralCategoryNames, nameOrValue)) return Property{Property::Kind::GeneralCategory, *i};
  if (auto i = findName(kBinaryNames, nameOrValue)) return Property{Property::Kind::Binary, *i};
  if (auto i = findName(kStringPropertyNames, nameOrValue)) return Property{Property::Kind::Strings, *i};
  return std::nullopt;
}

std::span<const CodePointRange> propertyCodePoints(Property p) {
  switch (p.kind) {
    case Property::Kind::GeneralCategory:
      return ranges(kGeneralCategoryRanges[p.index]);
    case Property::Kind::Script:
      return ranges(kScriptRanges[p.index]);
    case Property::Kind::ScriptExtensions:
      return ranges(kScriptExtensionsRanges[p.index]);
    case Property::Kind::Binary:
      return ranges(kBinaryRanges[p.index]);
    case Property::Kind::Strings:
      return ranges(kStringProperties[p.index].codePoints);
  }
  return {};
}

size_t propertyStringCount(Property p) {
  return p.kind == Property::Kind::Strings ? kStringProperties[p.index].stringCount : 0;
}

std::u32string_view propertyString(Property p, size_t i) {
  const StringRef& s = kStrings[kStringProperties[p.index].firstString + i];
  return {kStringPool + s.offset, s.length};
}

char32_t simpleCaseFold(char32_t cp) { return mapCase(kSimpleCaseFolding, cp); }
std::span<const CaseMapping> simpleCaseFoldings() { return kSimpleCaseFolding; }
char16_t legacyCanonicalize(char16_t cu) { return static_cast<char16_t>(mapCase(kLegacyCanonicalize, cu)); }
std::span<const CaseMapping> legacyCanonicalizations() { return kLegacyCanonicalize; }

}  // namespace snowman::unicode
