// String (22.1): the constructor, String.prototype (with Annex B's substr and HTML methods) and
// %StringIteratorPrototype%.
#include <algorithm>
#include <cmath>

#include "builtins/normalize.h"
#include "builtins/string_util.h"
#include "builtins/util.h"
#include "runtime/function.h"
#include "runtime/wrappers.h"
#include "unicode/unicode.h"

namespace snowman::builtins {

namespace {

struct CaseEntry {
  char32_t cp;
  uint8_t length;
  char32_t to[3];
};
#include "builtins/case_mapping.inc"

const CaseEntry* findCase(const CaseEntry* begin, const CaseEntry* end, char32_t cp) {
  const CaseEntry* it = std::lower_bound(begin, end, cp, [](const CaseEntry& e, char32_t c) { return e.cp < c; });
  return it != end && it->cp == cp ? it : nullptr;
}

void appendCodePoint(std::u16string& out, char32_t cp) {
  if (cp >= 0x10000) {
    cp -= 0x10000;
    out.push_back(char16_t(0xD800 + (cp >> 10)));
    out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
  } else {
    out.push_back(char16_t(cp));
  }
}

}  // namespace

RegExpCreateFn regExpCreate = nullptr;

CodePointRecord CodePointAt(std::u16string_view string, size_t position) {
  char16_t first = string[position];
  if (first < 0xD800 || first > 0xDFFF) return {first, 1, false};
  if (first >= 0xDC00 || position + 1 == string.size()) return {first, 1, true};
  char16_t second = string[position + 1];
  if (second < 0xDC00 || second > 0xDFFF) return {first, 1, true};
  return {0x10000 + ((char32_t(first) - 0xD800) << 10) + (char32_t(second) - 0xDC00), 2, false};
}

double StringIndexOf(std::u16string_view string, std::u16string_view searchValue, double fromIndex) {
  size_t len = string.size();
  if (searchValue.empty() && fromIndex <= double(len)) return fromIndex;
  if (fromIndex > double(len)) return -1;
  size_t pos = string.find(searchValue, size_t(fromIndex));
  return pos == std::u16string_view::npos ? -1 : double(pos);
}

Result<String*> GetSubstitution(String* matched, String* str, double position, const ValueVector& captures,
                                Value namedCaptures, String* replacementTemplate) {
  std::u16string_view s = str->view();
  size_t stringLength = s.size();
  std::u16string_view t = replacementTemplate->view();
  auto isDigit = [](char16_t c) { return c >= u'0' && c <= u'9'; };
  // Two walks over the template: the first adds up every piece but the named groups (whose Get is observable),
  // so a result over the string length limit is a RangeError before anything is allocated; the second builds it.
  std::u16string result;
  size_t length = 0;
  constexpr size_t kMaxLength = size_t(1) << 30;
  auto walk = [&](bool build) -> Result<void> {
    auto emit = [&](std::u16string_view piece) {
      if (build) result.append(piece);
      else length += piece.size();
    };
    size_t i = 0;
    while (i < t.size()) {
      std::u16string_view rest = t.substr(i);
      if (rest[0] != u'$' || rest.size() == 1) {
        emit(rest.substr(0, 1));
        i++;
        continue;
      }
      char16_t c = rest[1];
      if (c == u'$') {
        emit(u"$");
        i += 2;
      } else if (c == u'`') {
        emit(s.substr(0, size_t(position)));
        i += 2;
      } else if (c == u'&') {
        emit(matched->view());
        i += 2;
      } else if (c == u'\'') {
        size_t tailPos = std::min(size_t(position) + matched->length(), stringLength);
        emit(s.substr(tailPos));
        i += 2;
      } else if (isDigit(c)) {
        size_t digitCount = rest.size() > 2 && isDigit(rest[2]) ? 2 : 1;
        size_t index = digitCount == 2 ? size_t((rest[1] - u'0') * 10 + (rest[2] - u'0')) : size_t(rest[1] - u'0');
        size_t captureLen = captures.size();
        if (index > captureLen && digitCount == 2) {
          digitCount = 1;
          index = size_t(rest[1] - u'0');
        }
        std::u16string_view ref = rest.substr(0, 1 + digitCount);
        if (index >= 1 && index <= captureLen) {
          Value capture = captures[index - 1];
          if (!capture.isUndefined()) emit(capture.asString()->view());
        } else {
          emit(ref);
        }
        i += ref.size();
      } else if (c == u'<') {
        size_t gtPos = rest.find(u'>');
        if (gtPos == std::u16string_view::npos || namedCaptures.isUndefined()) {
          emit(u"$<");
          i += 2;
        } else {
          if (build) {
            std::u16string_view groupName = rest.substr(2, gtPos - 2);
            Value capture = TRY(Get(namedCaptures.asObject(), PropertyKey(groupName)));
            if (!capture.isUndefined()) {
              String* cs = TRY(ToString(capture));
              if (result.size() + cs->length() > kMaxLength) return ThrowRangeError("Invalid string length");
              emit(cs->view());
            }
          }
          i += gtPos + 1;
        }
      } else {
        emit(u"$");
        i++;
      }
      if (!build && length > kMaxLength) return ThrowRangeError("Invalid string length");
    }
    return {};
  };
  TRY(walk(false));
  result.reserve(length);
  TRY(walk(true));
  return newString(std::move(result));
}

namespace {

String* substring(String* s, size_t from, size_t to) {
  if (from == 0 && to == s->length()) return s;
  return newString(s->view().substr(from, to - from));
}

double clampD(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

// "Let O be ? RequireObjectCoercible(this value). Let S be ? ToString(O)."
Result<String*> thisString(Value thisValue) {
  TRY(RequireObjectCoercible(thisValue));
  return ToString(thisValue);
}

// 22.1.1.1 String ( value )
Result<Value> String_construct(NativeCall& call) {
  String* s;
  if (call.args.size() == 0) {
    s = emptyString();
  } else {
    Value value = call.args[0];
    if (!call.newTarget && value.isSymbol()) {
      Value desc = value.asSymbol()->description();
      return Value(concatStrings({u"Symbol(", desc.isUndefined() ? u"" : desc.asString()->view(), u")"}));
    }
    s = TRY(ToString(value));
  }
  if (!call.newTarget) return Value(s);
  Object* proto = TRY(GetPrototypeFromConstructor(call.newTarget, Intrinsic::StringPrototype));
  return Value(make<StringObject>(proto, s));
}

// 22.1.2.1 String.fromCharCode ( ...codeUnits )
Result<Value> String_fromCharCode(NativeCall& call) {
  std::u16string r;
  for (Value next : call.args) r.push_back(TRY(ToUint16(next)));
  return Value(newString(std::move(r)));
}

// 22.1.2.2 String.fromCodePoint ( ...codePoints )
Result<Value> String_fromCodePoint(NativeCall& call) {
  std::u16string r;
  for (Value next : call.args) {
    double nextCP = TRY(ToNumber(next));
    if (!IsIntegralNumber(num(nextCP)) || nextCP < 0 || nextCP > 0x10FFFF)
      return ThrowRangeError("Invalid code point " + NumberToString(nextCP)->toUtf8());
    appendCodePoint(r, char32_t(nextCP));
  }
  return Value(newString(std::move(r)));
}

// 22.1.2.4 String.raw ( template, ...substitutions )
Result<Value> String_raw(NativeCall& call) {
  size_t substitutionCount = call.args.size() > 0 ? call.args.size() - 1 : 0;
  Object* cooked = TRY(ToObject(call.args[0]));
  Value rawValue = TRY(Get(cooked, PropertyKey(u"raw")));
  Object* literals = TRY(ToObject(rawValue));
  double literalCount = TRY(LengthOfArrayLike(literals));
  if (literalCount <= 0) return Value(emptyString());
  std::u16string r;
  for (double nextIndex = 0;; nextIndex++) {
    Value nextLiteralVal = TRY(Get(literals, indexKey(nextIndex)));
    String* nextLiteral = TRY(ToString(nextLiteralVal));
    r.append(nextLiteral->view());
    if (nextIndex + 1 == literalCount) return Value(newString(std::move(r)));
    if (nextIndex < double(substitutionCount)) {
      String* nextSub = TRY(ToString(call.args[size_t(nextIndex) + 1]));
      r.append(nextSub->view());
    }
  }
}

// 22.1.3.35.1 ThisStringValue
Result<String*> thisStringValue(Value value) {
  if (value.isString()) return value.asString();
  if (value.isObject()) {
    if (auto* s = value.asObject()->as<StringObject>()) return s->stringData;
  }
  return ThrowTypeError("String.prototype method called on incompatible receiver");
}

Result<Value> StringPrototype_toString(NativeCall& call) { return Value(TRY(thisStringValue(call.thisValue))); }

// 22.1.3.1 String.prototype.at ( index )
Result<Value> StringPrototype_at(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  double len = double(s->length());
  double relativeIndex = TRY(ToIntegerOrInfinity(call.args[0]));
  double k = relativeIndex >= 0 ? relativeIndex : len + relativeIndex;
  if (k < 0 || k >= len) return Value::undefined();
  return Value(substring(s, size_t(k), size_t(k) + 1));
}

// 22.1.3.2 String.prototype.charAt ( pos )
Result<Value> StringPrototype_charAt(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  double position = TRY(ToIntegerOrInfinity(call.args[0]));
  if (position < 0 || position >= double(s->length())) return Value(emptyString());
  return Value(substring(s, size_t(position), size_t(position) + 1));
}

// 22.1.3.3 String.prototype.charCodeAt ( pos )
Result<Value> StringPrototype_charCodeAt(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  double position = TRY(ToIntegerOrInfinity(call.args[0]));
  if (position < 0 || position >= double(s->length())) return num(NAN);
  return num(s->at(size_t(position)));
}

// 22.1.3.4 String.prototype.codePointAt ( pos )
Result<Value> StringPrototype_codePointAt(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  double position = TRY(ToIntegerOrInfinity(call.args[0]));
  if (position < 0 || position >= double(s->length())) return Value::undefined();
  return num(CodePointAt(s->view(), size_t(position)).codePoint);
}

// 22.1.3.5 String.prototype.concat ( ...args )
Result<Value> StringPrototype_concat(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  std::u16string r(s->view());
  for (Value next : call.args) {
    String* nextString = TRY(ToString(next));
    r.append(nextString->view());
  }
  return Value(newString(std::move(r)));
}

Result<String*> searchStringArg(Value searchString) {
  bool isRegExp = TRY(IsRegExp(searchString));
  if (isRegExp) return ThrowTypeError("First argument must not be a regular expression");
  return ToString(searchString);
}

// 22.1.3.7 String.prototype.endsWith ( searchString [ , endPosition ] )
Result<Value> StringPrototype_endsWith(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  String* searchStr = TRY(searchStringArg(call.args[0]));
  double len = double(s->length());
  double pos = call.args[1].isUndefined() ? len : TRY(ToIntegerOrInfinity(call.args[1]));
  double end = clampD(pos, 0, len);
  double searchLength = double(searchStr->length());
  if (searchLength == 0) return boolean(true);
  double start = end - searchLength;
  if (start < 0) return boolean(false);
  return boolean(s->view().substr(size_t(start), size_t(searchLength)) == searchStr->view());
}

// 22.1.3.8 String.prototype.includes ( searchString [ , position ] )
Result<Value> StringPrototype_includes(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  String* searchStr = TRY(searchStringArg(call.args[0]));
  double pos = TRY(ToIntegerOrInfinity(call.args[1]));
  double start = clampD(pos, 0, double(s->length()));
  return boolean(StringIndexOf(s->view(), searchStr->view(), start) != -1);
}

// 22.1.3.9 String.prototype.indexOf ( searchString [ , position ] )
Result<Value> StringPrototype_indexOf(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  String* searchStr = TRY(ToString(call.args[0]));
  double pos = TRY(ToIntegerOrInfinity(call.args[1]));
  double start = clampD(pos, 0, double(s->length()));
  return num(StringIndexOf(s->view(), searchStr->view(), start));
}

// 22.1.3.10 String.prototype.isWellFormed ( )
Result<Value> StringPrototype_isWellFormed(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  std::u16string_view v = s->view();
  for (size_t i = 0; i < v.size();) {
    CodePointRecord cp = CodePointAt(v, i);
    if (cp.isUnpairedSurrogate) return boolean(false);
    i += size_t(cp.codeUnitCount);
  }
  return boolean(true);
}

// 22.1.3.11 String.prototype.lastIndexOf ( searchString [ , position ] )
Result<Value> StringPrototype_lastIndexOf(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  String* searchStr = TRY(ToString(call.args[0]));
  double numPos = TRY(ToNumber(call.args[1]));
  double pos = std::isnan(numPos) ? INFINITY : TRY(ToIntegerOrInfinity(num(numPos)));
  double len = double(s->length());
  double searchLen = double(searchStr->length());
  if (searchLen > len) return num(-1);
  double start = clampD(pos, 0, len - searchLen);
  std::u16string_view v = s->view(), sv = searchStr->view();
  for (double i = start; i >= 0; i--) {
    if (v.substr(size_t(i), sv.size()) == sv) return num(i);
  }
  return num(-1);
}

// 22.1.3.12 String.prototype.localeCompare ( that ): code point order of the NFD forms, so canonically
// equivalent strings compare equal.
Result<Value> StringPrototype_localeCompare(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  String* that = TRY(ToString(call.args[0]));
  std::u16string da = Normalize(s->view(), NormalizationForm::NFD), db = Normalize(that->view(), NormalizationForm::NFD);
  std::u16string_view a = da, b = db;
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    CodePointRecord x = CodePointAt(a, i), y = CodePointAt(b, j);
    if (x.codePoint != y.codePoint) return num(x.codePoint < y.codePoint ? -1 : 1);
    i += size_t(x.codeUnitCount);
    j += size_t(y.codeUnitCount);
  }
  if (i < a.size()) return num(1);
  if (j < b.size()) return num(-1);
  return num(0);
}

Result<Object*> createRegExp(Value pattern, Value flags) {
  if (!regExpCreate) return ThrowTypeError("RegExp is not supported");
  return regExpCreate(pattern, flags);
}

// 22.1.3.13 match, 22.1.3.24 search: dispatch on @@match / @@search, else a new RegExp.
Result<Value> regExpDispatch(NativeCall& call, Symbol* symbol) {
  Value o = call.thisValue;
  TRY(RequireObjectCoercible(o));
  Value regexp = call.args[0];
  if (regexp.isObject()) {
    Value method = TRY(GetMethod(regexp, PropertyKey(symbol)));
    if (!method.isUndefined()) return Call(method, regexp, {o});
  }
  String* s = TRY(ToString(o));
  Object* rx = TRY(createRegExp(regexp, Value::undefined()));
  return Invoke(rx, PropertyKey(symbol), {Value(s)});
}

Result<Value> StringPrototype_match(NativeCall& call) { return regExpDispatch(call, symbols().match); }
Result<Value> StringPrototype_search(NativeCall& call) { return regExpDispatch(call, symbols().search); }

// The flags check of matchAll and replaceAll.
Result<void> requireGlobalFlag(Value regexp) {
  bool isRegExp = TRY(IsRegExp(regexp));
  if (isRegExp) {
    Value flags = TRY(Get(regexp.asObject(), PropertyKey(u"flags")));
    TRY(RequireObjectCoercible(flags));
    String* f = TRY(ToString(flags));
    if (f->view().find(u'g') == std::u16string_view::npos)
      return ThrowTypeError("matchAll/replaceAll must be called with a global RegExp");
  }
  return {};
}

// 22.1.3.14 String.prototype.matchAll ( regexp )
Result<Value> StringPrototype_matchAll(NativeCall& call) {
  Value o = call.thisValue;
  TRY(RequireObjectCoercible(o));
  Value regexp = call.args[0];
  if (regexp.isObject()) {
    TRY(requireGlobalFlag(regexp));
    Value matcher = TRY(GetMethod(regexp, PropertyKey(symbols().matchAll)));
    if (!matcher.isUndefined()) return Call(matcher, regexp, {o});
  }
  String* s = TRY(ToString(o));
  Object* rx = TRY(createRegExp(regexp, str(u"g")));
  return Invoke(rx, PropertyKey(symbols().matchAll), {Value(s)});
}

// 22.1.3.15 String.prototype.normalize ( [ form ] )
Result<Value> StringPrototype_normalize(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  String* f = call.args[0].isUndefined() ? atom(u"NFC") : TRY(ToString(call.args[0]));
  std::u16string_view fv = f->view();
  if (fv != u"NFC" && fv != u"NFD" && fv != u"NFKC" && fv != u"NFKD")
    return ThrowRangeError("The normalization form should be one of NFC, NFD, NFKC, NFKD.");
  NormalizationForm form = fv == u"NFC" ? NormalizationForm::NFC : fv == u"NFD" ? NormalizationForm::NFD
                           : fv == u"NFKC" ? NormalizationForm::NFKC : NormalizationForm::NFKD;
  return Value(newString(Normalize(s->view(), form)));
}

// 22.1.3.17.1 StringPaddingBuiltinsImpl / StringPad
Result<Value> stringPad(NativeCall& call, bool atStart) {
  String* s = TRY(thisString(call.thisValue));
  double intMaxLength = TRY(ToLength(call.args[0]));
  double stringLength = double(s->length());
  if (intMaxLength <= stringLength) return Value(s);
  std::u16string_view filler = u" ";
  String* fillString = nullptr;
  if (!call.args[1].isUndefined()) {
    fillString = TRY(ToString(call.args[1]));
    filler = fillString->view();
  }
  if (filler.empty()) return Value(s);
  if (intMaxLength > double(1 << 30)) return ThrowRangeError("Invalid string length");
  size_t fillLen = size_t(intMaxLength - stringLength);
  std::u16string truncated;
  truncated.reserve(fillLen);
  while (truncated.size() < fillLen) truncated.append(filler.substr(0, fillLen - truncated.size()));
  if (atStart) return Value(newString(truncated + std::u16string(s->view())));
  return Value(newString(std::u16string(s->view()) + truncated));
}

Result<Value> StringPrototype_padEnd(NativeCall& call) { return stringPad(call, false); }
Result<Value> StringPrototype_padStart(NativeCall& call) { return stringPad(call, true); }

// 22.1.3.18 String.prototype.repeat ( count )
Result<Value> StringPrototype_repeat(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  double n = TRY(ToIntegerOrInfinity(call.args[0]));
  if (n < 0 || n == INFINITY) return ThrowRangeError("Invalid count value");
  if (n == 0 || s->length() == 0) return Value(emptyString());
  if (double(s->length()) * n > double(1 << 30)) return ThrowRangeError("Invalid string length");
  std::u16string r;
  r.reserve(size_t(double(s->length()) * n));
  for (double i = 0; i < n; i++) r.append(s->view());
  return Value(newString(std::move(r)));
}

// 22.1.3.19 String.prototype.replace ( searchValue, replaceValue )
Result<Value> StringPrototype_replace(NativeCall& call) {
  Value o = call.thisValue;
  TRY(RequireObjectCoercible(o));
  Value searchValue = call.args[0], replaceValue = call.args[1];
  if (searchValue.isObject()) {
    Value replacer = TRY(GetMethod(searchValue, PropertyKey(symbols().replace)));
    if (!replacer.isUndefined()) return Call(replacer, searchValue, {o, replaceValue});
  }
  String* string = TRY(ToString(o));
  String* searchString = TRY(ToString(searchValue));
  bool functionalReplace = IsCallable(replaceValue);
  String* replaceTemplate = nullptr;
  if (!functionalReplace) replaceTemplate = TRY(ToString(replaceValue));
  size_t searchLength = searchString->length();
  double position = StringIndexOf(string->view(), searchString->view(), 0);
  if (position == -1) return Value(string);
  std::u16string_view preceding = string->view().substr(0, size_t(position));
  std::u16string_view following = string->view().substr(size_t(position) + searchLength);
  String* replacement;
  if (functionalReplace) {
    Value r = TRY(Call(replaceValue, Value::undefined(), {Value(searchString), num(position), Value(string)}));
    replacement = TRY(ToString(r));
  } else {
    replacement = TRY(GetSubstitution(searchString, string, position, ValueVector(), Value::undefined(), replaceTemplate));
  }
  return Value(concatStrings({preceding, replacement->view(), following}));
}

// 22.1.3.20 String.prototype.replaceAll ( searchValue, replaceValue )
Result<Value> StringPrototype_replaceAll(NativeCall& call) {
  Value o = call.thisValue;
  TRY(RequireObjectCoercible(o));
  Value searchValue = call.args[0], replaceValue = call.args[1];
  if (searchValue.isObject()) {
    TRY(requireGlobalFlag(searchValue));
    Value replacer = TRY(GetMethod(searchValue, PropertyKey(symbols().replace)));
    if (!replacer.isUndefined()) return Call(replacer, searchValue, {o, replaceValue});
  }
  String* string = TRY(ToString(o));
  String* searchString = TRY(ToString(searchValue));
  bool functionalReplace = IsCallable(replaceValue);
  String* replaceTemplate = nullptr;
  if (!functionalReplace) replaceTemplate = TRY(ToString(replaceValue));
  size_t searchLength = searchString->length();
  double advanceBy = std::max<double>(1, double(searchLength));
  std::vector<double> matchPositions;
  double position = StringIndexOf(string->view(), searchString->view(), 0);
  while (position != -1) {
    matchPositions.push_back(position);
    position = StringIndexOf(string->view(), searchString->view(), position + advanceBy);
  }
  size_t endOfLastMatch = 0;
  std::u16string result;
  for (double p : matchPositions) {
    result.append(string->view().substr(endOfLastMatch, size_t(p) - endOfLastMatch));
    String* replacement;
    if (functionalReplace) {
      Value r = TRY(Call(replaceValue, Value::undefined(), {Value(searchString), num(p), Value(string)}));
      replacement = TRY(ToString(r));
    } else {
      replacement = TRY(GetSubstitution(searchString, string, p, ValueVector(), Value::undefined(), replaceTemplate));
    }
    result.append(replacement->view());
    endOfLastMatch = size_t(p) + searchLength;
  }
  if (endOfLastMatch < string->length()) result.append(string->view().substr(endOfLastMatch));
  return Value(newString(std::move(result)));
}

// 22.1.3.22 String.prototype.slice ( start, end )
Result<Value> StringPrototype_slice(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  double len = double(s->length());
  double from = relativeIndex(TRY(ToIntegerOrInfinity(call.args[0])), len);
  double intEnd = call.args[1].isUndefined() ? len : TRY(ToIntegerOrInfinity(call.args[1]));
  double to = relativeIndex(intEnd, len);
  if (from >= to) return Value(emptyString());
  return Value(substring(s, size_t(from), size_t(to)));
}

// 22.1.3.23 String.prototype.split ( separator, limit )
Result<Value> StringPrototype_split(NativeCall& call) {
  Value o = call.thisValue;
  TRY(RequireObjectCoercible(o));
  Value separator = call.args[0], limit = call.args[1];
  if (separator.isObject()) {
    Value splitter = TRY(GetMethod(separator, PropertyKey(symbols().split)));
    if (!splitter.isUndefined()) return Call(splitter, separator, {o, limit});
  }
  String* s = TRY(ToString(o));
  uint32_t lim = limit.isUndefined() ? 0xFFFFFFFFu : TRY(ToUint32(limit));
  String* r = TRY(ToString(separator));
  if (lim == 0) return Value(CreateArrayFromList({}));
  if (separator.isUndefined()) return Value(CreateArrayFromList({Value(s)}));
  size_t separatorLength = r->length();
  std::u16string_view sv = s->view();
  ValueVector substrings;
  if (separatorLength == 0) {
    size_t outLen = std::min<size_t>(lim, sv.size());
    for (size_t i = 0; i < outLen; i++) substrings.push_back(newString(sv.substr(i, 1)));
    return Value(CreateArrayFromList(substrings));
  }
  if (sv.empty()) return Value(CreateArrayFromList({Value(s)}));
  size_t i = 0;
  double j = StringIndexOf(sv, r->view(), 0);
  while (j != -1) {
    substrings.push_back(newString(sv.substr(i, size_t(j) - i)));
    if (substrings.size() == lim) return Value(CreateArrayFromList(substrings));
    i = size_t(j) + separatorLength;
    j = StringIndexOf(sv, r->view(), double(i));
  }
  substrings.push_back(newString(sv.substr(i)));
  return Value(CreateArrayFromList(substrings));
}

// 22.1.3.24 String.prototype.startsWith ( searchString [ , position ] )
Result<Value> StringPrototype_startsWith(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  String* searchStr = TRY(searchStringArg(call.args[0]));
  double len = double(s->length());
  double pos = TRY(ToIntegerOrInfinity(call.args[1]));
  double start = clampD(pos, 0, len);
  double searchLength = double(searchStr->length());
  if (searchLength == 0) return boolean(true);
  double end = start + searchLength;
  if (end > len) return boolean(false);
  return boolean(s->view().substr(size_t(start), size_t(searchLength)) == searchStr->view());
}

// 22.1.3.25 String.prototype.substring ( start, end )
Result<Value> StringPrototype_substring(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  double len = double(s->length());
  double intStart = TRY(ToIntegerOrInfinity(call.args[0]));
  double intEnd = call.args[1].isUndefined() ? len : TRY(ToIntegerOrInfinity(call.args[1]));
  double finalStart = clampD(intStart, 0, len), finalEnd = clampD(intEnd, 0, len);
  double from = std::min(finalStart, finalEnd), to = std::max(finalStart, finalEnd);
  return Value(substring(s, size_t(from), size_t(to)));
}

bool isCased(char32_t cp) {
  static auto property = unicode::lookupLoneProperty("Cased");
  if (!property) return false;
  auto ranges = unicode::propertyCodePoints(*property);
  auto it = std::upper_bound(ranges.begin(), ranges.end(), cp, [](char32_t c, const unicode::CodePointRange& r) { return c < r.first; });
  return it != ranges.begin() && cp <= (it - 1)->last;
}

bool isCaseIgnorable(char32_t cp) {
  static auto property = unicode::lookupLoneProperty("Case_Ignorable");
  if (!property) return false;
  auto ranges = unicode::propertyCodePoints(*property);
  auto it = std::upper_bound(ranges.begin(), ranges.end(), cp, [](char32_t c, const unicode::CodePointRange& r) { return c < r.first; });
  return it != ranges.begin() && cp <= (it - 1)->last;
}

// Final_Sigma (Unicode 3.13): Σ preceded by a cased letter and not followed by one (skipping case-ignorables).
bool isFinalSigma(std::u16string_view s, size_t at) {
  bool before = false;
  for (size_t i = at; i > 0;) {
    size_t j = i - 1;
    if (j > 0 && s[j] >= 0xDC00 && s[j] <= 0xDFFF && s[j - 1] >= 0xD800 && s[j - 1] <= 0xDBFF) j--;
    char32_t cp = CodePointAt(s, j).codePoint;
    i = j;
    if (isCaseIgnorable(cp)) continue;
    before = isCased(cp);
    break;
  }
  if (!before) return false;
  for (size_t i = at + 1; i < s.size();) {
    CodePointRecord r = CodePointAt(s, i);
    i += size_t(r.codeUnitCount);
    if (isCaseIgnorable(r.codePoint)) continue;
    return !isCased(r.codePoint);
  }
  return true;
}

// 22.1.3.28.1 TransformCase ( S, lower/upper )
String* transformCase(String* s, bool upper) {
  std::u16string_view v = s->view();
  bool ascii = true;
  for (char16_t c : v) {
    if (c >= 0x80) {
      ascii = false;
      break;
    }
  }
  std::u16string r;
  r.reserve(v.size());
  if (ascii) {
    for (char16_t c : v) {
      if (upper && c >= u'a' && c <= u'z') r.push_back(char16_t(c - 32));
      else if (!upper && c >= u'A' && c <= u'Z') r.push_back(char16_t(c + 32));
      else r.push_back(c);
    }
    return newString(std::move(r));
  }
  const CaseEntry* begin = upper ? std::begin(kUpperCase) : std::begin(kLowerCase);
  const CaseEntry* end = upper ? std::end(kUpperCase) : std::end(kLowerCase);
  for (size_t i = 0; i < v.size();) {
    CodePointRecord cp = CodePointAt(v, i);
    if (!upper && cp.codePoint == 0x03A3 && isFinalSigma(v, i)) {
      r.push_back(0x03C2);
    } else if (const CaseEntry* e = cp.isUnpairedSurrogate ? nullptr : findCase(begin, end, cp.codePoint)) {
      for (int k = 0; k < e->length; k++) appendCodePoint(r, e->to[k]);
    } else {
      r.append(v.substr(i, size_t(cp.codeUnitCount)));
    }
    i += size_t(cp.codeUnitCount);
  }
  return newString(std::move(r));
}

Result<Value> StringPrototype_toLowerCase(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  return Value(transformCase(s, false));
}

Result<Value> StringPrototype_toUpperCase(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  return Value(transformCase(s, true));
}

// 22.1.3.31 String.prototype.toWellFormed ( )
Result<Value> StringPrototype_toWellFormed(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  std::u16string_view v = s->view();
  std::u16string r;
  r.reserve(v.size());
  for (size_t i = 0; i < v.size();) {
    CodePointRecord cp = CodePointAt(v, i);
    if (cp.isUnpairedSurrogate) r.push_back(0xFFFD);
    else r.append(v.substr(i, size_t(cp.codeUnitCount)));
    i += size_t(cp.codeUnitCount);
  }
  return Value(newString(std::move(r)));
}

// 22.1.3.32.1 TrimString ( string, where )
Result<Value> trimString(Value thisValue, bool start, bool end) {
  String* s = TRY(thisString(thisValue));
  std::u16string_view v = s->view();
  size_t b = 0, e = v.size();
  if (start) {
    while (b < e && isWhiteSpaceOrLineTerminator(v[b])) b++;
  }
  if (end) {
    while (e > b && isWhiteSpaceOrLineTerminator(v[e - 1])) e--;
  }
  return Value(substring(s, b, e));
}

Result<Value> StringPrototype_trim(NativeCall& call) { return trimString(call.thisValue, true, true); }
Result<Value> StringPrototype_trimEnd(NativeCall& call) { return trimString(call.thisValue, false, true); }
Result<Value> StringPrototype_trimStart(NativeCall& call) { return trimString(call.thisValue, true, false); }

// %StringIteratorPrototype% objects (22.1.5).
class StringIterator : public Object {
 public:
  StringIterator(Object* prototype, String* s) : Object(prototype), string(s) {}
  String* string;  // nullptr once exhausted
  size_t position = 0;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(reinterpret_cast<Cell*>(string));
  }
};

// 22.1.3.36 String.prototype [ @@iterator ] ( )
Result<Value> StringPrototype_iterator(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  return Value(make<StringIterator>(intrinsic(Intrinsic::StringIteratorPrototype), s));
}

// 22.1.5.1.1 %StringIteratorPrototype%.next ( )
Result<Value> StringIteratorPrototype_next(NativeCall& call) {
  StringIterator* it = call.thisValue.isObject() ? call.thisValue.asObject()->as<StringIterator>() : nullptr;
  if (!it) return ThrowTypeError("next method called on incompatible receiver");
  if (!it->string || it->position >= it->string->length()) {
    it->string = nullptr;
    return Value(CreateIteratorResultObject(Value::undefined(), true));
  }
  CodePointRecord cp = CodePointAt(it->string->view(), it->position);
  String* result = substring(it->string, it->position, it->position + size_t(cp.codeUnitCount));
  it->position += size_t(cp.codeUnitCount);
  return Value(CreateIteratorResultObject(Value(result), false));
}

// B.2.2.1 String.prototype.substr ( start, length )
Result<Value> StringPrototype_substr(NativeCall& call) {
  String* s = TRY(thisString(call.thisValue));
  double size = double(s->length());
  double intStart = TRY(ToIntegerOrInfinity(call.args[0]));
  if (intStart == -INFINITY) intStart = 0;
  else if (intStart < 0) intStart = std::max(size + intStart, 0.0);
  else intStart = std::min(intStart, size);
  double intLength = call.args[1].isUndefined() ? size : TRY(ToIntegerOrInfinity(call.args[1]));
  intLength = clampD(intLength, 0, size);
  double intEnd = std::min(intStart + intLength, size);
  if (intStart >= intEnd) return Value(emptyString());
  return Value(substring(s, size_t(intStart), size_t(intEnd)));
}

// B.2.2.2.1 CreateHTML ( string, tag, attribute, value )
Result<Value> CreateHTML(Value string, std::u16string_view tag, std::u16string_view attribute, Value value) {
  String* s = TRY(thisString(string));
  std::u16string p = u"<";
  p.append(tag);
  if (!attribute.empty()) {
    String* v = TRY(ToString(value));
    p.push_back(u' ');
    p.append(attribute);
    p.append(u"=\"");
    for (char16_t c : v->view()) {
      if (c == u'"') p.append(u"&quot;");
      else p.push_back(c);
    }
    p.push_back(u'"');
  }
  p.push_back(u'>');
  p.append(s->view());
  p.append(u"</");
  p.append(tag);
  p.push_back(u'>');
  return Value(newString(std::move(p)));
}

#define SNOWMAN_HTML_METHOD(fn, tag, attr) \
  Result<Value> fn(NativeCall& call) { return CreateHTML(call.thisValue, tag, attr, call.args[0]); }
SNOWMAN_HTML_METHOD(StringPrototype_anchor, u"a", u"name")
SNOWMAN_HTML_METHOD(StringPrototype_big, u"big", u"")
SNOWMAN_HTML_METHOD(StringPrototype_blink, u"blink", u"")
SNOWMAN_HTML_METHOD(StringPrototype_bold, u"b", u"")
SNOWMAN_HTML_METHOD(StringPrototype_fixed, u"tt", u"")
SNOWMAN_HTML_METHOD(StringPrototype_fontcolor, u"font", u"color")
SNOWMAN_HTML_METHOD(StringPrototype_fontsize, u"font", u"size")
SNOWMAN_HTML_METHOD(StringPrototype_italics, u"i", u"")
SNOWMAN_HTML_METHOD(StringPrototype_link, u"a", u"href")
SNOWMAN_HTML_METHOD(StringPrototype_small, u"small", u"")
SNOWMAN_HTML_METHOD(StringPrototype_strike, u"strike", u"")
SNOWMAN_HTML_METHOD(StringPrototype_sub, u"sub", u"")
SNOWMAN_HTML_METHOD(StringPrototype_sup, u"sup", u"")
#undef SNOWMAN_HTML_METHOD

void installString(Realm& realm) {
  Object* proto = make<StringObject>(realm.intrinsic(Intrinsic::ObjectPrototype), emptyString());
  BuiltinFunction* ctor =
      defineConstructor(realm, Intrinsic::String, String_construct, 1, PropertyKey(u"String"), proto, Intrinsic::StringPrototype);
  defineFunction(realm, ctor, PropertyKey(u"fromCharCode"), 1, String_fromCharCode);
  defineFunction(realm, ctor, PropertyKey(u"fromCodePoint"), 1, String_fromCodePoint);
  defineFunction(realm, ctor, PropertyKey(u"raw"), 1, String_raw);

  struct Fn {
    const char16_t* name;
    int length;
    NativeFn fn;
  };
  const Fn fns[] = {
      {u"at", 1, StringPrototype_at},
      {u"charAt", 1, StringPrototype_charAt},
      {u"charCodeAt", 1, StringPrototype_charCodeAt},
      {u"codePointAt", 1, StringPrototype_codePointAt},
      {u"concat", 1, StringPrototype_concat},
      {u"endsWith", 1, StringPrototype_endsWith},
      {u"includes", 1, StringPrototype_includes},
      {u"indexOf", 1, StringPrototype_indexOf},
      {u"isWellFormed", 0, StringPrototype_isWellFormed},
      {u"lastIndexOf", 1, StringPrototype_lastIndexOf},
      {u"localeCompare", 1, StringPrototype_localeCompare},
      {u"match", 1, StringPrototype_match},
      {u"matchAll", 1, StringPrototype_matchAll},
      {u"normalize", 0, StringPrototype_normalize},
      {u"padEnd", 1, StringPrototype_padEnd},
      {u"padStart", 1, StringPrototype_padStart},
      {u"repeat", 1, StringPrototype_repeat},
      {u"replace", 2, StringPrototype_replace},
      {u"replaceAll", 2, StringPrototype_replaceAll},
      {u"search", 1, StringPrototype_search},
      {u"slice", 2, StringPrototype_slice},
      {u"split", 2, StringPrototype_split},
      {u"startsWith", 1, StringPrototype_startsWith},
      {u"substring", 2, StringPrototype_substring},
      {u"toLocaleLowerCase", 0, StringPrototype_toLowerCase},
      {u"toLocaleUpperCase", 0, StringPrototype_toUpperCase},
      {u"toLowerCase", 0, StringPrototype_toLowerCase},
      {u"toString", 0, StringPrototype_toString},
      {u"toUpperCase", 0, StringPrototype_toUpperCase},
      {u"toWellFormed", 0, StringPrototype_toWellFormed},
      {u"trim", 0, StringPrototype_trim},
      {u"valueOf", 0, StringPrototype_toString},
      {u"substr", 2, StringPrototype_substr},
      {u"anchor", 1, StringPrototype_anchor},
      {u"big", 0, StringPrototype_big},
      {u"blink", 0, StringPrototype_blink},
      {u"bold", 0, StringPrototype_bold},
      {u"fixed", 0, StringPrototype_fixed},
      {u"fontcolor", 1, StringPrototype_fontcolor},
      {u"fontsize", 1, StringPrototype_fontsize},
      {u"italics", 0, StringPrototype_italics},
      {u"link", 1, StringPrototype_link},
      {u"small", 0, StringPrototype_small},
      {u"strike", 0, StringPrototype_strike},
      {u"sub", 0, StringPrototype_sub},
      {u"sup", 0, StringPrototype_sup},
  };
  for (const Fn& f : fns) defineFunction(realm, proto, PropertyKey(f.name), f.length, f.fn);
  BuiltinFunction* trimEnd = defineFunction(realm, proto, PropertyKey(u"trimEnd"), 0, StringPrototype_trimEnd);
  BuiltinFunction* trimStart = defineFunction(realm, proto, PropertyKey(u"trimStart"), 0, StringPrototype_trimStart);
  defineValue(proto, PropertyKey(u"trimLeft"), trimStart);
  defineValue(proto, PropertyKey(u"trimRight"), trimEnd);
  defineFunction(realm, proto, PropertyKey(symbols().iterator), 0, StringPrototype_iterator);

  Object* iterProto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::IteratorPrototype));
  realm.setIntrinsic(Intrinsic::StringIteratorPrototype, iterProto);
  defineFunction(realm, iterProto, PropertyKey(u"next"), 0, StringIteratorPrototype_next);
  defineValue(iterProto, PropertyKey(symbols().toStringTag), str(u"String Iterator"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(String, kOrderDefault, installString);

}  // namespace snowman::builtins
