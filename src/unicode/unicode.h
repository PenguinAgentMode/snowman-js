// Unicode character data the lexer and the RegExp engine share. Tables are generated from node's Unicode
// data by tools/unicode/ (Unicode 17.0, the version test262's generated tests use).
#pragma once

namespace snowman::unicode {

// ID_Start / ID_Continue (UAX #31). ECMAScript adds $ and _ (start) and ZWNJ/ZWJ (part) itself.
bool isIDStart(char32_t cp);
bool isIDContinue(char32_t cp);
// General_Category=Space_Separator (Zs): part of ECMAScript WhiteSpace.
bool isSpaceSeparator(char32_t cp);

}  // namespace snowman::unicode
