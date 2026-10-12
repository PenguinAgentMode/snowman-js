// JS strings: immutable sequences of UTF-16 code units, lone surrogates allowed. INTERFACES.md section 4.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "runtime/heap.h"

namespace snowman {

class String final : public Cell {
 public:
  explicit String(std::u16string units) : units_(std::move(units)) {}

  std::u16string_view view() const { return units_; }
  size_t length() const { return units_.size(); }
  char16_t at(size_t index) const { return units_[index]; }
  bool isAtom() const { return atom_; }
  size_t hash() const;
  // UTF-8 for printing; lone surrogates become U+FFFD.
  std::string toUtf8() const;

 private:
  friend String* atom(std::u16string_view units);
  std::u16string units_;
  mutable size_t hash_ = 0;
  bool atom_ = false;
};

// New strings. The empty string is a shared atom.
String* newString(std::u16string_view units);
String* newString(std::u16string&& units);
String* newStringFromUtf8(std::string_view utf8);
String* emptyString();
// Interned strings: equal contents give the same pointer. Atoms are immortal. Property-key strings are atoms.
String* atom(std::u16string_view units);
String* atom(String* string);
String* atomFromUtf8(std::string_view utf8);

bool equals(const String* a, const String* b);
// a + b
String* concat(String* a, String* b);

// UTF-8 <-> UTF-16 helpers shared by the runtime and the CLI.
std::u16string utf8ToUtf16(std::string_view utf8);
std::string utf16ToUtf8(std::u16string_view utf16);

}  // namespace snowman
