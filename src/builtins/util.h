// Shared helpers for the built-in files (src/builtins/, runtime workstream).
#pragma once

#include <string>
#include <string_view>

#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/property_map.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/vm.h"

namespace snowman::builtins {

// The running execution context's realm (inside a native: the built-in's own realm).
inline Realm* currentRealm() { return vm().currentRealm(); }
inline Object* intrinsic(Intrinsic id) { return vm().currentRealm()->intrinsic(id); }
inline const WellKnownSymbols& symbols() { return vm().symbols; }

inline Value str(std::u16string_view s) { return Value(atom(s)); }
inline Value num(double d) { return Value::number(d); }
inline Value boolean(bool b) { return Value::boolean(b); }

// A fresh ordinary object inheriting from the current realm's %Object.prototype%.
inline Object* newPlainObject() { return OrdinaryObjectCreate(intrinsic(Intrinsic::ObjectPrototype)); }

// "If NewTarget is undefined, let newTarget be the active function object."
inline Object* newTargetOrCallee(NativeCall& call) {
  return call.newTarget ? call.newTarget : static_cast<Object*>(call.callee);
}

// IfAbruptCloseIterator: close the iterator, then rethrow `thrown`.
inline Throw closeIteratorWithThrow(const IteratorRecord& record, Value thrown) {
  Result<void> r = IteratorClose(record, Throw{thrown});
  return Throw{r.thrown()};
}

// TRY that closes `record` on a throw (IfAbruptCloseIterator).
#define TRY_CLOSE(record, ...)                                                                   \
  ({                                                                                             \
    auto _snowman_try = (__VA_ARGS__);                                                           \
    if (_snowman_try.isThrow()) return ::snowman::builtins::closeIteratorWithThrow(record, _snowman_try.thrown()); \
    _snowman_try.release();                                                                      \
  })

// Clamp a relative index (ToIntegerOrInfinity result) into [0, len], as slice/splice/fill etc. do.
inline double relativeIndex(double relative, double len) {
  if (relative < 0) return relative + len < 0 ? 0 : relative + len;
  return relative > len ? len : relative;
}

inline String* concatStrings(std::initializer_list<std::u16string_view> parts) {
  std::u16string out;
  for (auto p : parts) out.append(p);
  return newString(std::move(out));
}

// The property key for an integer index that may exceed the array-index range (array-likes up to 2^53 - 1).
inline PropertyKey indexKey(double index) {
  if (index < double(PropertyKey::kMaxIndex) + 1) return PropertyKey::index(uint32_t(index));
  return PropertyKey(NumberToString(index));
}

inline std::u16string toU16(std::string_view ascii) { return std::u16string(ascii.begin(), ascii.end()); }

}  // namespace snowman::builtins
