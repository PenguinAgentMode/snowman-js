// PropertyKey canonicalization.
#include "runtime/property.h"

#include <string>

#include "runtime/string.h"
#include "runtime/symbol.h"

namespace snowman {

namespace {

// A canonical array index: "0" or [1-9][0-9]* with a value ≤ 2^32 − 2.
bool parseArrayIndex(std::u16string_view s, uint32_t& out) {
  if (s.empty() || s.size() > 10) return false;
  if (s[0] == u'0') {
    if (s.size() != 1) return false;
    out = 0;
    return true;
  }
  uint64_t v = 0;
  for (char16_t c : s) {
    if (c < u'0' || c > u'9') return false;
    v = v * 10 + (c - u'0');
  }
  if (v > PropertyKey::kMaxIndex) return false;
  out = uint32_t(v);
  return true;
}

String* indexString(uint32_t index) {
  static String* small[1024] = {};
  if (index < 1024 && small[index]) return small[index];
  char buf[16];
  int n = snprintf(buf, sizeof buf, "%u", index);
  std::u16string units(buf, buf + n);
  String* s = index < 1024 ? atom(units) : newString(std::move(units));
  if (index < 1024) small[index] = s;
  return s;
}

}  // namespace

PropertyKey::PropertyKey(String* string) {
  uint32_t index;
  if (parseArrayIndex(string->view(), index)) {
    kind_ = Kind::Index;
    u_.index = index;
  } else {
    kind_ = Kind::String;
    u_.string = atom(string);
  }
}

PropertyKey::PropertyKey(std::u16string_view name) {
  uint32_t index;
  if (parseArrayIndex(name, index)) {
    kind_ = Kind::Index;
    u_.index = index;
  } else {
    kind_ = Kind::String;
    u_.string = atom(name);
  }
}

Value PropertyKey::toValue() const {
  switch (kind_) {
    case Kind::Index:
      return indexString(u_.index);
    case Kind::String:
      return u_.string;
    case Kind::Symbol:
      return u_.symbol;
  }
  return Value();
}

size_t PropertyKey::hash() const {
  uint64_t h;
  if (kind_ == Kind::Index) h = uint64_t(u_.index) * 0x9E3779B97F4A7C15ull;
  else h = (reinterpret_cast<uintptr_t>(u_.string) >> 3) * 0x9E3779B97F4A7C15ull + uint64_t(kind_);
  return size_t(h ^ (h >> 29));
}

}  // namespace snowman
