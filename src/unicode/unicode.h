// Unicode character data the lexer and the RegExp engine share. Tables are generated from node's Unicode
// data by tools/unicode/ (Unicode 17.0, the version test262's generated tests use).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace snowman::unicode {

// ID_Start / ID_Continue (UAX #31). ECMAScript adds $ and _ (start) and ZWNJ/ZWJ (part) itself.
bool isIDStart(char32_t cp);
bool isIDContinue(char32_t cp);
// General_Category=Space_Separator (Zs): part of ECMAScript WhiteSpace.
bool isSpaceSeparator(char32_t cp);

// An inclusive code point range. Range lists are sorted, disjoint and never adjacent.
struct CodePointRange {
  char32_t first;
  char32_t last;
};

// A property \p{...} may name (ECMA-262 2025 tables 67-69 and PropertyValueAliases.txt).
struct Property {
  enum class Kind : uint8_t { GeneralCategory, Script, ScriptExtensions, Binary, Strings };
  Kind kind;
  uint16_t index;
};

// \p{name=value}: name is General_Category/gc, Script/sc or Script_Extensions/scx and value one of its values
// or value aliases, matched exactly (no loose matching).
std::optional<Property> lookupProperty(std::string_view name, std::string_view value);
// \p{nameOrValue}: a General_Category value or alias, a binary property or alias, or a binary property of
// strings (Kind::Strings, which only the v flag allows: the caller checks).
std::optional<Property> lookupLoneProperty(std::string_view nameOrValue);
// The code points that have the property; for Kind::Strings, its strings of exactly one code point.
std::span<const CodePointRange> propertyCodePoints(Property property);
// Kind::Strings only: its strings of two or more code points, longest first.
size_t propertyStringCount(Property property);
std::u32string_view propertyString(Property property, size_t i);

// Canonicalize (ECMA-262 22.2.2.7.3) with IgnoreCase. A from -> to mapping lists every character that does
// not map to itself, sorted by from.
struct CaseMapping {
  char32_t from;
  char32_t to;
};
// With the u or v flag: simple case folding (CaseFolding.txt statuses C and S).
char32_t simpleCaseFold(char32_t cp);
std::span<const CaseMapping> simpleCaseFoldings();
// Without: toUppercase, if that is a single code unit and does not map a non-ASCII unit to ASCII.
char16_t legacyCanonicalize(char16_t cu);
std::span<const CaseMapping> legacyCanonicalizations();

}  // namespace snowman::unicode
