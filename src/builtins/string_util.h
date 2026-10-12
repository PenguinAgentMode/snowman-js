// String helpers shared between String.prototype and RegExp.prototype (src/builtins/).
#pragma once

#include <optional>
#include <string_view>

#include "runtime/object.h"
#include "runtime/result.h"
#include "runtime/string.h"

namespace snowman::builtins {

// 6.1.4.1 StringIndexOf ( string, searchValue, fromIndex ): -1 for not-found.
double StringIndexOf(std::u16string_view string, std::u16string_view searchValue, double fromIndex);

// 22.1.3.19.1 GetSubstitution ( matched, str, position, captures, namedCaptures, replacementTemplate )
// captures holds undefined or Strings; namedCaptures is undefined or an Object.
Result<String*> GetSubstitution(String* matched, String* str, double position, const ValueVector& captures,
                                Value namedCaptures, String* replacementTemplate);

// 22.2.3.1 RegExpCreate ( P, F ): provided by the RegExp built-in through this hook (nullptr until then).
using RegExpCreateFn = Result<Object*> (*)(Value pattern, Value flags);
extern RegExpCreateFn regExpCreate;

// 11.1.4 CodePointAt ( string, position )
struct CodePointRecord {
  char32_t codePoint;
  int codeUnitCount;
  bool isUnpairedSurrogate;
};
CodePointRecord CodePointAt(std::u16string_view string, size_t position);

}  // namespace snowman::builtins
