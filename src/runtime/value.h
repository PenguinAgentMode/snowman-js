// Value: the tagged union every JS value is. INTERFACES.md section 4.
#pragma once

#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include "runtime/heap.h"

namespace snowman {

class String;
class Symbol;
class BigInt;
class Object;

enum class ValueType : uint8_t { Undefined, Null, Boolean, Number, String, Symbol, BigInt, Object, Empty };

class Value {
 public:
  constexpr Value() : type_(ValueType::Undefined), u_{.cell = nullptr} {}
  // Non-null cell pointers convert implicitly. There is deliberately no constructor from bool or double:
  // use Value::boolean / Value::number, so a stray pointer never becomes a boolean.
  Value(String* string) : type_(ValueType::String), u_{.cell = reinterpret_cast<Cell*>(string)} {}
  Value(Symbol* symbol) : type_(ValueType::Symbol), u_{.cell = reinterpret_cast<Cell*>(symbol)} {}
  Value(BigInt* bigint) : type_(ValueType::BigInt), u_{.cell = reinterpret_cast<Cell*>(bigint)} {}
  template <class T>
    requires std::derived_from<T, Object>
  Value(T* object) : type_(ValueType::Object), u_{.cell = reinterpret_cast<Cell*>(static_cast<Object*>(object))} {}

  static constexpr Value undefined() { return Value(); }
  static constexpr Value null() { return Value(ValueType::Null); }
  // ~empty~: array holes and uninitialized bindings. Never visible to JS code.
  static constexpr Value empty() { return Value(ValueType::Empty); }
  static constexpr Value boolean(bool b) {
    Value v(ValueType::Boolean);
    v.u_.boolean = b;
    return v;
  }
  static constexpr Value number(double d) {
    Value v(ValueType::Number);
    v.u_.number = d;
    return v;
  }

  ValueType type() const { return type_; }
  bool isUndefined() const { return type_ == ValueType::Undefined; }
  bool isNull() const { return type_ == ValueType::Null; }
  bool isNullish() const { return type_ == ValueType::Undefined || type_ == ValueType::Null; }
  bool isBoolean() const { return type_ == ValueType::Boolean; }
  bool isNumber() const { return type_ == ValueType::Number; }
  bool isString() const { return type_ == ValueType::String; }
  bool isSymbol() const { return type_ == ValueType::Symbol; }
  bool isBigInt() const { return type_ == ValueType::BigInt; }
  bool isObject() const { return type_ == ValueType::Object; }
  bool isEmpty() const { return type_ == ValueType::Empty; }
  bool isCell() const {
    return type_ == ValueType::String || type_ == ValueType::Symbol || type_ == ValueType::BigInt ||
           type_ == ValueType::Object;
  }

  bool asBoolean() const { return u_.boolean; }
  double asNumber() const { return u_.number; }
  String* asString() const { return reinterpret_cast<String*>(u_.cell); }
  Symbol* asSymbol() const { return reinterpret_cast<Symbol*>(u_.cell); }
  BigInt* asBigInt() const { return reinterpret_cast<BigInt*>(u_.cell); }
  Object* asObject() const { return reinterpret_cast<Object*>(u_.cell); }
  Cell* asCell() const { return isCell() ? u_.cell : nullptr; }

 private:
  explicit constexpr Value(ValueType type) : type_(type), u_{.cell = nullptr} {}

  ValueType type_;
  union {
    bool boolean;
    double number;
    Cell* cell;
  } u_;
};

inline void Tracer::visit(const Value& value) {
  if (Cell* cell = value.asCell()) visitCell(cell);
}

// ValueVector: a rooted list of values for C++ code outside cells (argument lists, CreateListFromArrayLike,
// IteratorToList). Converts to Args (object.h).
class ValueVector : public RootProvider {
 public:
  ValueVector() = default;
  ValueVector(std::initializer_list<Value> values) : values_(values) {}
  explicit ValueVector(size_t count) : values_(count) {}

  size_t size() const { return values_.size(); }
  bool empty() const { return values_.empty(); }
  Value& operator[](size_t i) { return values_[i]; }
  const Value& operator[](size_t i) const { return values_[i]; }
  void push_back(Value value) { values_.push_back(value); }
  void pop_back() { values_.pop_back(); }
  void clear() { values_.clear(); }
  void reserve(size_t n) { values_.reserve(n); }
  void resize(size_t n) { values_.resize(n); }
  Value* data() { return values_.data(); }
  const Value* data() const { return values_.data(); }
  auto begin() { return values_.begin(); }
  auto end() { return values_.end(); }
  auto begin() const { return values_.begin(); }
  auto end() const { return values_.end(); }
  std::vector<Value>& vector() { return values_; }
  const std::vector<Value>& vector() const { return values_; }

  void traceRoots(Tracer& tracer) override { tracer.visitAll(values_); }

 private:
  std::vector<Value> values_;
};

// Rooted<T>: keeps one cell pointer alive from off-heap C++ memory.
template <class T>
class Rooted : public RootProvider {
 public:
  explicit Rooted(T* cell = nullptr) : cell_(cell) {}
  T* get() const { return cell_; }
  T* operator->() const { return cell_; }
  void set(T* cell) { cell_ = cell; }
  void traceRoots(Tracer& tracer) override { tracer.visit(reinterpret_cast<Cell*>(cell_)); }

 private:
  T* cell_;
};

}  // namespace snowman
