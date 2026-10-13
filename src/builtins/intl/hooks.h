// The ECMA-402 versions of the locale-sensitive ECMA-262 methods (ECMA-402 19-21), called from src/builtins/
// string.cpp, number.cpp, bigint.cpp, date.cpp, array.cpp and typedarray.cpp.
#pragma once

#include <string>

#include "runtime/object.h"
#include "runtime/result.h"
#include "runtime/string.h"

namespace snowman::intl {

// 19.1.1 String.prototype.localeCompare ( that [ , locales [ , options ] ] ), after the ToString steps.
Result<Value> localeCompare(String* s, String* that, Value locales, Value options);

// 19.1.2/3 TransformCase ( S, locales, targetCase ): the language whose case mappings apply ("tr", "az", "lt" or
// ""), then the mapping, given the locale-independent full case mapping of the caller.
Result<std::string> transformCaseLanguage(Value locales);
using CaseMapping = String* (*)(String* s, bool upper);
String* transformCase(String* s, const std::string& language, bool upper, CaseMapping defaultMapping);

// 20.2.1 Number.prototype.toLocaleString / 21.1 BigInt.prototype.toLocaleString: x is a Number or a BigInt.
Result<Value> numberToLocaleString(Value x, Value locales, Value options);

// 21.2 Date.prototype.toLocaleString / toLocaleDateString / toLocaleTimeString on a time value.
enum class DateRequired : uint8_t { Any, Date, Time };
Result<Value> dateToLocaleString(double timeValue, Value locales, Value options, DateRequired required);

}  // namespace snowman::intl
