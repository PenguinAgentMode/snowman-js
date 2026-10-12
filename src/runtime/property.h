// Property keys, descriptors and attributes. INTERFACES.md section 5.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "runtime/value.h"

namespace snowman {

// An array index (a canonical numeric string ≤ 2^32 − 2), an atom (any other string), or a Symbol.
// Constructing from a String canonicalizes, so "1" and index(1) are the same key.
class PropertyKey {
 public:
  static constexpr uint32_t kMaxIndex = 0xFFFFFFFEu;

  static PropertyKey index(uint32_t index) {
    PropertyKey key;
    key.kind_ = Kind::Index;
    key.u_.index = index;
    return key;
  }
  PropertyKey(String* string);  // canonicalizes; atomizes non-index strings
  PropertyKey(Symbol* symbol) : kind_(Kind::Symbol) { u_.symbol = symbol; }
  PropertyKey(std::u16string_view name);  // e.g. PropertyKey(u"length")
  PropertyKey(const char16_t* name) : PropertyKey(std::u16string_view(name)) {}

  bool isIndex() const { return kind_ == Kind::Index; }
  bool isString() const { return kind_ == Kind::String; }  // a non-index string
  bool isSymbol() const { return kind_ == Kind::Symbol; }
  uint32_t asIndex() const { return u_.index; }
  String* asString() const { return u_.string; }  // an atom
  Symbol* asSymbol() const { return u_.symbol; }

  // The key as a JS value: a String (indices are converted) or a Symbol.
  Value toValue() const;
  size_t hash() const;
  bool operator==(const PropertyKey& other) const {
    return kind_ == other.kind_ && (kind_ == Kind::Index ? u_.index == other.u_.index : u_.string == other.u_.string);
  }

  void trace(Tracer& tracer) const {
    if (kind_ == Kind::Symbol) tracer.visit(reinterpret_cast<Cell*>(u_.symbol));
  }

 private:
  enum class Kind : uint8_t { Index, String, Symbol };
  PropertyKey() = default;

  Kind kind_ = Kind::Index;
  union {
    uint32_t index;
    String* string;
    Symbol* symbol;
  } u_{};
};

struct PropertyKeyHash {
  size_t operator()(const PropertyKey& key) const { return key.hash(); }
};

enum class Attr : uint8_t {
  None = 0,
  Writable = 1,
  Enumerable = 2,
  Configurable = 4,
  WritableConfigurable = 5,  // built-in methods and data properties
  All = 7,                   // ordinary assignment-created properties
};
constexpr Attr operator|(Attr a, Attr b) { return Attr(uint8_t(a) | uint8_t(b)); }
constexpr bool has(Attr attrs, Attr flag) { return (uint8_t(attrs) & uint8_t(flag)) == uint8_t(flag); }

// A Property Descriptor (6.2.6): every field may be absent. get/set hold undefined or a callable object.
struct PropertyDescriptor {
  std::optional<Value> value;
  std::optional<Value> get;
  std::optional<Value> set;
  std::optional<bool> writable;
  std::optional<bool> enumerable;
  std::optional<bool> configurable;

  bool isAccessorDescriptor() const { return get.has_value() || set.has_value(); }
  bool isDataDescriptor() const { return value.has_value() || writable.has_value(); }
  bool isGenericDescriptor() const { return !isAccessorDescriptor() && !isDataDescriptor(); }

  static PropertyDescriptor data(Value value, Attr attrs) {
    return {value, {}, {}, has(attrs, Attr::Writable), has(attrs, Attr::Enumerable), has(attrs, Attr::Configurable)};
  }
  static PropertyDescriptor accessor(Value getter, Value setter, Attr attrs) {
    return {{}, getter, setter, {}, has(attrs, Attr::Enumerable), has(attrs, Attr::Configurable)};
  }
};

}  // namespace snowman
