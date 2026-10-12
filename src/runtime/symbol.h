// Symbols. Well-known symbols are shared by every realm (vm().symbols). INTERFACES.md section 4.
#pragma once

#include "runtime/value.h"

namespace snowman {

class Symbol final : public Cell {
 public:
  // description: undefined or a String.
  explicit Symbol(Value description) : description_(description) {}
  Value description() const { return description_; }
  void trace(Tracer& tracer) override { tracer.visit(description_); }

 private:
  Value description_;
};

#define SNOWMAN_WELL_KNOWN_SYMBOLS(X) \
  X(asyncDispose)                     \
  X(asyncIterator)                    \
  X(dispose)                          \
  X(hasInstance)                      \
  X(isConcatSpreadable)               \
  X(iterator)                         \
  X(match)                            \
  X(matchAll)                         \
  X(replace)                          \
  X(search)                           \
  X(species)                          \
  X(split)                            \
  X(toPrimitive)                      \
  X(toStringTag)                      \
  X(unscopables)

struct WellKnownSymbols {
#define SNOWMAN_DECLARE_SYMBOL(name) Symbol* name = nullptr;
  SNOWMAN_WELL_KNOWN_SYMBOLS(SNOWMAN_DECLARE_SYMBOL)
#undef SNOWMAN_DECLARE_SYMBOL
};

}  // namespace snowman
