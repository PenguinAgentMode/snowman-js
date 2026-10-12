// Completion records: Result<T> is a normal completion holding a T, or a throw completion holding the
// thrown Value. INTERFACES.md section 7. Break/continue/return completions belong to the interpreter.
#pragma once

#include <optional>
#include <type_traits>
#include <utility>

#include "runtime/value.h"

namespace snowman {

// A throw completion. Converts to any Result<T>, so `return ThrowTypeError("...");` works everywhere.
struct Throw {
  Value value;
};

template <class T>
class [[nodiscard]] Result {
 public:
  template <class U>
    requires(std::is_constructible_v<T, U &&> && !std::is_same_v<std::remove_cvref_t<U>, Result> &&
             !std::is_same_v<std::remove_cvref_t<U>, Throw>)
  Result(U&& value) : value_(std::forward<U>(value)) {}
  Result(Throw thrown) : thrown_(thrown.value) {}

  bool isThrow() const { return !value_.has_value(); }
  bool isNormal() const { return value_.has_value(); }

  T& value() & { return *value_; }
  const T& value() const& { return *value_; }
  T release() { return std::move(*value_); }
  Value thrown() const { return thrown_; }

 private:
  std::optional<T> value_;
  Value thrown_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Throw thrown) : throw_(true), thrown_(thrown.value) {}

  bool isThrow() const { return throw_; }
  bool isNormal() const { return !throw_; }
  void release() {}
  Value thrown() const { return thrown_; }

 private:
  bool throw_ = false;
  Value thrown_;
};

}  // namespace snowman

// TRY(expr): the normal value of a Result, or return its throw from the enclosing function.
#define TRY(...)                                                                      \
  ({                                                                                  \
    auto _snowman_try = (__VA_ARGS__);                                                \
    if (_snowman_try.isThrow()) return ::snowman::Throw{_snowman_try.thrown()};       \
    _snowman_try.release();                                                           \
  })
