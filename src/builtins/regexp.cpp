// RegExp (22.2): the constructor, RegExp.prototype (exec, test, the Symbol.* methods, the flag accessors,
// Annex B compile), RegExp.escape and %RegExpStringIteratorPrototype%. Matching is src/regexp/'s.
#include <algorithm>
#include <cmath>
#include <memory>

#include "builtins/string_util.h"
#include "builtins/util.h"
#include "regexp/regexp.h"
#include "runtime/array.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

class RegExpObject : public Object {
 public:
  explicit RegExpObject(Object* prototype) : Object(prototype) {}
  std::shared_ptr<const regexp::Program> matcher;  // [[RegExpMatcher]]; null until RegExpInitialize
  String* originalSource = nullptr;                // [[OriginalSource]]
  String* originalFlags = nullptr;                 // [[OriginalFlags]]
  Realm* realm = nullptr;                          // [[Realm]] (legacy RegExp features)
  bool legacyFeaturesEnabled = false;              // [[LegacyFeaturesEnabled]]
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(reinterpret_cast<Cell*>(originalSource));
    tracer.visit(reinterpret_cast<Cell*>(originalFlags));
    tracer.visit(realm);
  }
};

const PropertyKey& lastIndexKey() {
  static const PropertyKey key(u"lastIndex");
  return key;
}

// 22.2.3.2 RegExpAlloc ( newTarget ), with the legacy RegExp features' [[Realm]] and [[LegacyFeaturesEnabled]]
Result<RegExpObject*> RegExpAlloc(Object* newTarget) {
  RegExpObject* obj = TRY(OrdinaryCreateFromConstructor<RegExpObject>(newTarget, Intrinsic::RegExpPrototype));
  obj->realm = currentRealm();
  obj->legacyFeaturesEnabled = newTarget == obj->realm->intrinsic(Intrinsic::RegExp);
  TRY(DefinePropertyOrThrow(obj, lastIndexKey(), PropertyDescriptor::data(Value::undefined(), Attr::Writable)));
  return obj;
}

// 22.2.3.3 RegExpInitialize ( obj, pattern, flags )
Result<Object*> RegExpInitialize(RegExpObject* obj, Value pattern, Value flags) {
  String* p = pattern.isUndefined() ? emptyString() : TRY(ToString(pattern));
  String* f = flags.isUndefined() ? emptyString() : TRY(ToString(flags));
  std::string error;
  std::shared_ptr<const regexp::Program> program = regexp::compile(p->view(), f->view(), &error);
  if (!program) return ThrowSyntaxError(error);
  obj->originalSource = p;
  obj->originalFlags = f;
  obj->matcher = std::move(program);
  TRY(Set(obj, lastIndexKey(), num(0), true));
  return static_cast<Object*>(obj);
}

}  // namespace

// 22.2.3.1 RegExpCreate ( P, F )
Result<Object*> RegExpCreate(Value pattern, Value flags) {
  RegExpObject* obj = TRY(RegExpAlloc(intrinsic(Intrinsic::RegExp)));
  return RegExpInitialize(obj, pattern, flags);
}

namespace {

RegExpObject* asRegExp(Value v) { return v.isObject() ? v.asObject()->as<RegExpObject>() : nullptr; }

// 22.2.4.1 RegExp ( pattern, flags )
Result<Value> RegExp_construct(NativeCall& call) {
  Value pattern = call.args[0], flags = call.args[1];
  bool patternIsRegExp = TRY(IsRegExp(pattern));
  Object* newTarget = call.newTarget;
  if (!newTarget) {
    newTarget = call.callee;
    if (patternIsRegExp && flags.isUndefined()) {
      Value patternConstructor = TRY(Get(pattern.asObject(), PropertyKey(u"constructor")));
      if (patternConstructor.isObject() && patternConstructor.asObject() == newTarget) return pattern;
    }
  }
  Value p, f;
  if (RegExpObject* rx = asRegExp(pattern)) {
    p = Value(rx->originalSource);
    f = flags.isUndefined() ? Value(rx->originalFlags) : flags;
  } else if (patternIsRegExp) {
    p = TRY(Get(pattern.asObject(), PropertyKey(u"source")));
    f = flags.isUndefined() ? TRY(Get(pattern.asObject(), PropertyKey(u"flags"))) : flags;
  } else {
    p = pattern;
    f = flags;
  }
  RegExpObject* o = TRY(RegExpAlloc(newTarget));
  return Value(TRY(RegExpInitialize(o, p, f)));
}

Result<Value> RegExp_species(NativeCall& call) { return call.thisValue; }

// 22.2.6.4.1 RegExpHasFlag ( R, codeUnit )
template <char16_t C>
Result<Value> RegExpHasFlag(NativeCall& call) {
  Value r = call.thisValue;
  if (!r.isObject()) return ThrowTypeError("RegExp flag getter called on non-object");
  RegExpObject* rx = asRegExp(r);
  if (!rx || !rx->originalFlags) {
    if (r.asObject() == intrinsic(Intrinsic::RegExpPrototype)) return Value::undefined();
    return ThrowTypeError("RegExp flag getter called on incompatible receiver");
  }
  return boolean(rx->originalFlags->view().find(C) != std::u16string_view::npos);
}

// 22.2.6.4 get RegExp.prototype.flags
Result<Value> RegExpPrototype_flags(NativeCall& call) {
  Value r = call.thisValue;
  if (!r.isObject()) return ThrowTypeError("RegExp.prototype.flags getter called on non-object");
  static const std::pair<const char16_t*, char16_t> flags[] = {
      {u"hasIndices", u'd'}, {u"global", u'g'}, {u"ignoreCase", u'i'}, {u"multiline", u'm'},
      {u"dotAll", u's'},     {u"unicode", u'u'}, {u"unicodeSets", u'v'}, {u"sticky", u'y'},
  };
  std::u16string codeUnits;
  for (auto& [name, unit] : flags) {
    if (ToBoolean(TRY(Get(r.asObject(), PropertyKey(name))))) codeUnits.push_back(unit);
  }
  return Value(newString(std::move(codeUnits)));
}

// 22.2.6.13.1 EscapeRegExpPattern ( P, F )
String* EscapeRegExpPattern(String* source) {
  std::u16string_view s = source->view();
  if (s.empty()) return atom(u"(?:)");
  std::u16string out;
  bool inClass = false;
  for (size_t i = 0; i < s.size(); i++) {
    char16_t c = s[i];
    if (c == u'\\' && i + 1 < s.size()) {
      char16_t next = s[++i];
      out.push_back(c);
      if (next == u'\n') out.push_back(u'n');
      else if (next == u'\r') out.push_back(u'r');
      else if (next == 0x2028) out.append(u"u2028");
      else if (next == 0x2029) out.append(u"u2029");
      else out.push_back(next);
      continue;
    }
    if (c == u'[') inClass = true;
    else if (c == u']') inClass = false;
    if (c == u'/' && !inClass) out.append(u"\\/");
    else if (c == u'\n') out.append(u"\\n");
    else if (c == u'\r') out.append(u"\\r");
    else if (c == 0x2028) out.append(u"\\u2028");
    else if (c == 0x2029) out.append(u"\\u2029");
    else out.push_back(c);
  }
  return newString(std::move(out));
}

// 22.2.6.13 get RegExp.prototype.source
Result<Value> RegExpPrototype_source(NativeCall& call) {
  Value r = call.thisValue;
  if (!r.isObject()) return ThrowTypeError("RegExp.prototype.source getter called on non-object");
  RegExpObject* rx = asRegExp(r);
  if (!rx || !rx->originalSource) {
    if (r.asObject() == intrinsic(Intrinsic::RegExpPrototype)) return str(u"(?:)");
    return ThrowTypeError("RegExp.prototype.source getter called on incompatible receiver");
  }
  return Value(EscapeRegExpPattern(rx->originalSource));
}

// 22.2.6.17 RegExp.prototype.toString ( )
Result<Value> RegExpPrototype_toString(NativeCall& call) {
  Value r = call.thisValue;
  if (!r.isObject()) return ThrowTypeError("RegExp.prototype.toString called on non-object");
  String* pattern = TRY(ToString(TRY(Get(r.asObject(), PropertyKey(u"source")))));
  String* flags = TRY(ToString(TRY(Get(r.asObject(), PropertyKey(u"flags")))));
  return Value(concatStrings({u"/", pattern->view(), u"/", flags->view()}));
}

// B.2.4.1 RegExp.prototype.compile ( pattern, flags ), with the legacy RegExp features' receiver checks
Result<Value> RegExpPrototype_compile(NativeCall& call) {
  RegExpObject* o = asRegExp(call.thisValue);
  if (!o) return ThrowTypeError("RegExp.prototype.compile called on incompatible receiver");
  if (o->realm != currentRealm()) return ThrowTypeError("RegExp.prototype.compile called on a RegExp of another realm");
  if (!o->legacyFeaturesEnabled) return ThrowTypeError("RegExp.prototype.compile called on a RegExp subclass instance");
  Value pattern = call.args[0], flags = call.args[1];
  Value p = pattern, f = flags;
  if (RegExpObject* rx = asRegExp(pattern)) {
    if (!flags.isUndefined()) return ThrowTypeError("RegExp.prototype.compile: flags must be undefined for a RegExp pattern");
    p = Value(rx->originalSource);
    f = Value(rx->originalFlags);
  }
  return Value(TRY(RegExpInitialize(o, p, f)));
}

// 22.2.5.1 RegExp.escape ( S )
Result<Value> RegExp_escape(NativeCall& call) {
  Value s = call.args[0];
  if (!s.isString()) return ThrowTypeError("RegExp.escape requires a string");
  static const char hex[] = "0123456789abcdef";
  auto hexEscape = [&](std::u16string& out, char32_t c) {
    if (c <= 0xFF) {
      out.append(u"\\x");
      out.push_back(char16_t(hex[c >> 4]));
      out.push_back(char16_t(hex[c & 15]));
      return;
    }
    auto unit = [&](char16_t u) {
      out.append(u"\\u");
      for (int shift = 12; shift >= 0; shift -= 4) out.push_back(char16_t(hex[(u >> shift) & 15]));
    };
    if (c >= 0x10000) {
      unit(char16_t(0xD800 + ((c - 0x10000) >> 10)));
      unit(char16_t(0xDC00 + ((c - 0x10000) & 0x3FF)));
    } else {
      unit(char16_t(c));
    }
  };
  std::u16string_view v = s.asString()->view();
  std::u16string_view syntax = u"^$\\.*+?()[]{}|/";
  std::u16string_view other = u",-=<>#&!%:;@~'`\"";
  std::u16string out;
  for (size_t i = 0; i < v.size();) {
    CodePointRecord cp = CodePointAt(v, i);
    char32_t c = cp.codePoint;
    i += size_t(cp.codeUnitCount);
    bool isAlnum = (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
    if (out.empty() && isAlnum) {
      hexEscape(out, c);
    } else if (c < 0x10000 && syntax.find(char16_t(c)) != std::u16string_view::npos) {
      out.push_back(u'\\');
      out.push_back(char16_t(c));
    } else if (c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D) {
      static const char16_t letters[] = u"tnvfr";
      out.push_back(u'\\');
      out.push_back(letters[c - 0x09]);
    } else if ((c < 0x10000 && other.find(char16_t(c)) != std::u16string_view::npos) ||
               (c < 0x10000 && isWhiteSpaceOrLineTerminator(char16_t(c))) || (c >= 0xD800 && c <= 0xDFFF)) {
      hexEscape(out, c);
    } else if (c >= 0x10000) {
      out.append(v.substr(i - 2, 2));
    } else {
      out.push_back(char16_t(c));
    }
  }
  return Value(newString(std::move(out)));
}

// 22.2.7.3 AdvanceStringIndex ( S, index, unicode )
double AdvanceStringIndex(String* s, double index, bool unicode) {
  if (!unicode) return index + 1;
  double length = double(s->length());
  if (index + 1 >= length) return index + 1;
  return index + double(CodePointAt(s->view(), size_t(index)).codeUnitCount);
}

// 22.2.7.8 MakeMatchIndicesIndexPairArray ( S, indices, groupNames, hasGroups )
Object* MakeMatchIndicesIndexPairArray(const std::vector<regexp::Span>& indices, const std::vector<String*>& groupNames,
                                       bool hasGroups) {
  ArrayObject* a = make<ArrayObject>(intrinsic(Intrinsic::ArrayPrototype), uint32_t(indices.size()));
  Value groups = hasGroups ? Value(OrdinaryObjectCreate(nullptr)) : Value::undefined();
  (void)CreateDataProperty(a, PropertyKey(u"groups"), groups);
  for (size_t i = 0; i < indices.size(); i++) {
    Value pair;
    if (indices[i].start >= 0) pair = Value(CreateArrayFromList({num(indices[i].start), num(indices[i].end)}));
    (void)CreateDataProperty(a, PropertyKey::index(uint32_t(i)), pair);
    if (i > 0 && groupNames[i - 1]) (void)CreateDataProperty(groups.asObject(), PropertyKey(groupNames[i - 1]), pair);
  }
  return a;
}

// ---- Legacy RegExp static properties (proposal-regexp-legacy-features) ----
// %RegExp%'s [[RegExpInput]], [[RegExpLastMatch]], ... live in the constructor's slots. The match-derived ones are
// kept as the last match's subject and spans and become strings when read.
enum LegacySlot : size_t {
  kLegacyInput,       // [[RegExpInput]]: a String, or empty once invalidated
  kLegacySubject,     // the last match's S, or empty once invalidated
  kLegacyMatchStart,  // [[RegExpLastMatch]] = S[start, end); leftContext/rightContext around it
  kLegacyMatchEnd,
  kLegacyLastParen,   // [[RegExpLastParen]]: start, end (-1 = the empty String)
  kLegacyParen1 = kLegacyLastParen + 2,  // [[RegExpParen1..9]]: start, end pairs
  kLegacySlotCount = kLegacyParen1 + 18,
};

BuiltinFunction* legacyRegExpConstructor() { return static_cast<BuiltinFunction*>(intrinsic(Intrinsic::RegExp)); }

void setLegacySpan(std::vector<Value>& slots, size_t at, const regexp::Span* span) {
  slots[at] = num(span && span->start >= 0 ? span->start : -1);
  slots[at + 1] = num(span && span->start >= 0 ? span->end : -1);
}

// UpdateLegacyRegExpStaticProperties ( C, S, startIndex, endIndex, capturedValues )
void UpdateLegacyRegExpStaticProperties(String* s, const std::vector<regexp::Span>& spans, uint32_t n) {
  std::vector<Value>& slots = legacyRegExpConstructor()->slots;
  slots[kLegacyInput] = Value(s);
  slots[kLegacySubject] = Value(s);
  slots[kLegacyMatchStart] = num(spans[0].start);
  slots[kLegacyMatchEnd] = num(spans[0].end);
  setLegacySpan(slots, kLegacyLastParen, n > 0 ? &spans[n] : nullptr);
  for (uint32_t i = 1; i <= 9; i++) setLegacySpan(slots, kLegacyParen1 + 2 * (i - 1), i <= n ? &spans[i] : nullptr);
}

// InvalidateLegacyRegExpStaticProperties ( C )
void InvalidateLegacyRegExpStaticProperties() {
  std::vector<Value>& slots = legacyRegExpConstructor()->slots;
  slots[kLegacyInput] = Value::empty();
  slots[kLegacySubject] = Value::empty();
}

Value legacySubstring(String* s, Value start, Value end) {
  if (start.asNumber() < 0) return Value(emptyString());
  return Value(newString(s->view().substr(size_t(start.asNumber()), size_t(end.asNumber() - start.asNumber()))));
}

enum class LegacyProperty { Input, LastMatch, LastParen, LeftContext, RightContext, Paren1 };

// GetLegacyRegExpStaticProperty ( C, thisValue, internalSlotName ); Paren1 + i - 1 is [[RegExpPareni]].
template <LegacyProperty Property, int Paren = 0>
Result<Value> GetLegacyRegExpStaticProperty(NativeCall& call) {
  BuiltinFunction* c = legacyRegExpConstructor();
  if (!call.thisValue.isObject() || call.thisValue.asObject() != c)
    return ThrowTypeError("RegExp legacy static property read on a receiver other than %RegExp%");
  const std::vector<Value>& slots = c->slots;
  if (Property == LegacyProperty::Input) {
    if (slots[kLegacyInput].isEmpty()) return ThrowTypeError("RegExp legacy static properties are not available");
    return slots[kLegacyInput];
  }
  if (slots[kLegacySubject].isEmpty()) return ThrowTypeError("RegExp legacy static properties are not available");
  String* s = slots[kLegacySubject].asString();
  switch (Property) {
    case LegacyProperty::LastMatch:
      return legacySubstring(s, slots[kLegacyMatchStart], slots[kLegacyMatchEnd]);
    case LegacyProperty::LastParen:
      return legacySubstring(s, slots[kLegacyLastParen], slots[kLegacyLastParen + 1]);
    case LegacyProperty::LeftContext:
      return legacySubstring(s, num(0), slots[kLegacyMatchStart]);
    case LegacyProperty::RightContext:
      return legacySubstring(s, slots[kLegacyMatchEnd], num(double(s->length())));
    default: {
      size_t at = kLegacyParen1 + 2 * size_t(Paren - 1);
      return legacySubstring(s, slots[at], slots[at + 1]);
    }
  }
}

// SetLegacyRegExpStaticProperty ( C, thisValue, internalSlotName, val ) for [[RegExpInput]]
Result<Value> SetLegacyRegExpInput(NativeCall& call) {
  BuiltinFunction* c = legacyRegExpConstructor();
  if (!call.thisValue.isObject() || call.thisValue.asObject() != c)
    return ThrowTypeError("RegExp legacy static property set on a receiver other than %RegExp%");
  String* strVal = TRY(ToString(call.args[0]));
  c->slots[kLegacyInput] = Value(strVal);
  return Value::undefined();
}

void installLegacyRegExpStaticProperties(Realm& realm, BuiltinFunction* ctor) {
  ctor->slots.assign(kLegacySlotCount, num(-1));
  ctor->slots[kLegacyInput] = Value(emptyString());
  ctor->slots[kLegacySubject] = Value(emptyString());
  ctor->slots[kLegacyMatchStart] = num(0);
  ctor->slots[kLegacyMatchEnd] = num(0);
  auto both = [&](const char16_t* name, const char16_t* alias, NativeFn getter, NativeFn setter) {
    defineAccessor(realm, ctor, PropertyKey(name), getter, setter);
    defineAccessor(realm, ctor, PropertyKey(alias), getter, setter);
  };
  both(u"input", u"$_", GetLegacyRegExpStaticProperty<LegacyProperty::Input>, SetLegacyRegExpInput);
  both(u"lastMatch", u"$&", GetLegacyRegExpStaticProperty<LegacyProperty::LastMatch>, nullptr);
  both(u"lastParen", u"$+", GetLegacyRegExpStaticProperty<LegacyProperty::LastParen>, nullptr);
  both(u"leftContext", u"$`", GetLegacyRegExpStaticProperty<LegacyProperty::LeftContext>, nullptr);
  both(u"rightContext", u"$'", GetLegacyRegExpStaticProperty<LegacyProperty::RightContext>, nullptr);
  defineAccessor(realm, ctor, PropertyKey(u"$1"), GetLegacyRegExpStaticProperty<LegacyProperty::Paren1, 1>, nullptr);
  defineAccessor(realm, ctor, PropertyKey(u"$2"), GetLegacyRegExpStaticProperty<LegacyProperty::Paren1, 2>, nullptr);
  defineAccessor(realm, ctor, PropertyKey(u"$3"), GetLegacyRegExpStaticProperty<LegacyProperty::Paren1, 3>, nullptr);
  defineAccessor(realm, ctor, PropertyKey(u"$4"), GetLegacyRegExpStaticProperty<LegacyProperty::Paren1, 4>, nullptr);
  defineAccessor(realm, ctor, PropertyKey(u"$5"), GetLegacyRegExpStaticProperty<LegacyProperty::Paren1, 5>, nullptr);
  defineAccessor(realm, ctor, PropertyKey(u"$6"), GetLegacyRegExpStaticProperty<LegacyProperty::Paren1, 6>, nullptr);
  defineAccessor(realm, ctor, PropertyKey(u"$7"), GetLegacyRegExpStaticProperty<LegacyProperty::Paren1, 7>, nullptr);
  defineAccessor(realm, ctor, PropertyKey(u"$8"), GetLegacyRegExpStaticProperty<LegacyProperty::Paren1, 8>, nullptr);
  defineAccessor(realm, ctor, PropertyKey(u"$9"), GetLegacyRegExpStaticProperty<LegacyProperty::Paren1, 9>, nullptr);
}

// 22.2.7.2 RegExpBuiltinExec ( R, S )
Result<Value> RegExpBuiltinExec(RegExpObject* r, String* s) {
  double length = double(s->length());
  double lastIndex = TRY(ToLength(TRY(Get(r, lastIndexKey()))));
  const regexp::Program& program = *r->matcher;
  const regexp::Flags& flags = regexp::flagsOf(program);
  bool global = flags.global, sticky = flags.sticky, hasIndices = flags.hasIndices;
  if (!global && !sticky) lastIndex = 0;
  if (lastIndex > length) {
    if (global || sticky) TRY(Set(r, lastIndexKey(), num(0), true));
    return Value::null();
  }
  std::vector<regexp::Span> spans;
  regexp::ExecStatus status = regexp::exec(program, s->view(), size_t(lastIndex), &spans);
  if (status == regexp::ExecStatus::ResourceExhausted) return ThrowRangeError("Maximum regular expression backtracking exceeded");
  if (status == regexp::ExecStatus::NoMatch) {
    if (global || sticky) TRY(Set(r, lastIndexKey(), num(0), true));
    return Value::null();
  }
  double matchStart = spans[0].start, e = spans[0].end;
  if (global || sticky) TRY(Set(r, lastIndexKey(), num(e), true));
  uint32_t n = regexp::captureCount(program);
  ArrayObject* a = make<ArrayObject>(intrinsic(Intrinsic::ArrayPrototype), n + 1);
  (void)CreateDataProperty(a, PropertyKey(u"index"), num(matchStart));
  (void)CreateDataProperty(a, PropertyKey(u"input"), Value(s));
  (void)CreateDataProperty(a, PropertyKey::index(0), Value(newString(s->view().substr(size_t(matchStart), size_t(e - matchStart)))));
  bool hasGroups = regexp::hasGroupNames(program);
  Value groups = hasGroups ? Value(OrdinaryObjectCreate(nullptr)) : Value::undefined();
  (void)CreateDataProperty(a, PropertyKey(u"groups"), groups);
  if (r->realm == currentRealm()) {
    if (r->legacyFeaturesEnabled) UpdateLegacyRegExpStaticProperties(s, spans, n);
    else InvalidateLegacyRegExpStaticProperties();
  }
  std::vector<String*> groupNames;
  std::vector<std::u16string> matchedGroupNames;
  for (uint32_t i = 1; i <= n; i++) {
    const regexp::Span& span = spans[i];
    Value capturedValue;
    if (span.start >= 0) capturedValue = Value(newString(s->view().substr(size_t(span.start), size_t(span.end - span.start))));
    (void)CreateDataProperty(a, PropertyKey::index(i), capturedValue);
    const std::u16string& name = regexp::groupName(program, i);
    if (name.empty()) {
      groupNames.push_back(nullptr);
      continue;
    }
    if (std::find(matchedGroupNames.begin(), matchedGroupNames.end(), name) != matchedGroupNames.end()) {
      groupNames.push_back(nullptr);
      continue;
    }
    if (!capturedValue.isUndefined()) matchedGroupNames.push_back(name);
    (void)CreateDataProperty(groups.asObject(), PropertyKey(name), capturedValue);
    groupNames.push_back(atom(name));
  }
  if (hasIndices) {
    std::vector<regexp::Span> indices(spans.begin(), spans.begin() + n + 1);
    indices[0].start = int32_t(matchStart);
    (void)CreateDataProperty(a, PropertyKey(u"indices"), Value(MakeMatchIndicesIndexPairArray(indices, groupNames, hasGroups)));
  }
  return Value(a);
}

// 22.2.7.1 RegExpExec ( R, S )
Result<Value> RegExpExec(Object* r, String* s) {
  Value exec = TRY(Get(r, PropertyKey(u"exec")));
  if (IsCallable(exec)) {
    Value result = TRY(Call(exec, r, {Value(s)}));
    if (!result.isObject() && !result.isNull()) return ThrowTypeError("RegExp exec method returned something other than an Object or null");
    return result;
  }
  RegExpObject* rx = r->as<RegExpObject>();
  if (!rx) return ThrowTypeError("RegExp.prototype.exec called on incompatible receiver");
  return RegExpBuiltinExec(rx, s);
}

// 22.2.6.2 RegExp.prototype.exec ( string )
Result<Value> RegExpPrototype_exec(NativeCall& call) {
  RegExpObject* r = asRegExp(call.thisValue);
  if (!r || !r->matcher) return ThrowTypeError("RegExp.prototype.exec called on incompatible receiver");
  String* s = TRY(ToString(call.args[0]));
  return RegExpBuiltinExec(r, s);
}

Result<Object*> thisObject(Value v, const char* method) {
  if (!v.isObject()) return ThrowTypeError(std::string("RegExp.prototype") + method + " called on non-object");
  return v.asObject();
}

// 22.2.6.16 RegExp.prototype.test ( S )
Result<Value> RegExpPrototype_test(NativeCall& call) {
  Object* r = TRY(thisObject(call.thisValue, ".test"));
  String* s = TRY(ToString(call.args[0]));
  Value match = TRY(RegExpExec(r, s));
  return boolean(!match.isNull());
}

Result<String*> flagsOfObject(Object* rx) { return ToString(TRY(Get(rx, PropertyKey(u"flags")))); }
bool hasUnit(String* flags, char16_t unit) { return flags->view().find(unit) != std::u16string_view::npos; }

// 22.2.6.8 RegExp.prototype [ @@match ] ( string )
Result<Value> RegExpPrototype_match(NativeCall& call) {
  Object* rx = TRY(thisObject(call.thisValue, "[Symbol.match]"));
  String* s = TRY(ToString(call.args[0]));
  String* flags = TRY(flagsOfObject(rx));
  if (!hasUnit(flags, u'g')) return RegExpExec(rx, s);
  bool fullUnicode = hasUnit(flags, u'u') || hasUnit(flags, u'v');
  TRY(Set(rx, lastIndexKey(), num(0), true));
  Object* a = TRY(ArrayCreate(0));
  double n = 0;
  while (true) {
    Value result = TRY(RegExpExec(rx, s));
    if (result.isNull()) return n == 0 ? Value::null() : Value(a);
    String* matchStr = TRY(ToString(TRY(Get(result.asObject(), PropertyKey::index(0)))));
    TRY(CreateDataPropertyOrThrow(a, indexKey(n), Value(matchStr)));
    if (matchStr->length() == 0) {
      double thisIndex = TRY(ToLength(TRY(Get(rx, lastIndexKey()))));
      TRY(Set(rx, lastIndexKey(), num(AdvanceStringIndex(s, thisIndex, fullUnicode)), true));
    }
    n++;
  }
}

// 22.2.6.12 RegExp.prototype [ @@search ] ( string )
Result<Value> RegExpPrototype_search(NativeCall& call) {
  Object* rx = TRY(thisObject(call.thisValue, "[Symbol.search]"));
  String* s = TRY(ToString(call.args[0]));
  Value previousLastIndex = TRY(Get(rx, lastIndexKey()));
  if (!SameValue(previousLastIndex, num(0))) TRY(Set(rx, lastIndexKey(), num(0), true));
  Value result = TRY(RegExpExec(rx, s));
  Value currentLastIndex = TRY(Get(rx, lastIndexKey()));
  if (!SameValue(currentLastIndex, previousLastIndex)) TRY(Set(rx, lastIndexKey(), previousLastIndex, true));
  if (result.isNull()) return num(-1);
  return Get(result.asObject(), PropertyKey(u"index"));
}

// 22.2.6.11 RegExp.prototype [ @@replace ] ( string, replaceValue )
Result<Value> RegExpPrototype_replace(NativeCall& call) {
  Object* rx = TRY(thisObject(call.thisValue, "[Symbol.replace]"));
  String* s = TRY(ToString(call.args[0]));
  double lengthS = double(s->length());
  Value replaceValue = call.args[1];
  bool functionalReplace = IsCallable(replaceValue);
  String* replaceTemplate = nullptr;
  if (!functionalReplace) replaceTemplate = TRY(ToString(replaceValue));
  String* flags = TRY(flagsOfObject(rx));
  bool global = hasUnit(flags, u'g');
  bool fullUnicode = false;
  if (global) {
    fullUnicode = hasUnit(flags, u'u') || hasUnit(flags, u'v');
    TRY(Set(rx, lastIndexKey(), num(0), true));
  }
  ValueVector results;
  while (true) {
    Value result = TRY(RegExpExec(rx, s));
    if (result.isNull()) break;
    results.push_back(result);
    if (!global) break;
    String* matchStr = TRY(ToString(TRY(Get(result.asObject(), PropertyKey::index(0)))));
    if (matchStr->length() == 0) {
      double thisIndex = TRY(ToLength(TRY(Get(rx, lastIndexKey()))));
      TRY(Set(rx, lastIndexKey(), num(AdvanceStringIndex(s, thisIndex, fullUnicode)), true));
    }
  }
  std::u16string accumulated;
  double nextSourcePosition = 0;
  for (Value resultValue : results) {
    Object* result = resultValue.asObject();
    double resultLength = TRY(LengthOfArrayLike(result));
    double nCaptures = std::max(resultLength - 1, 0.0);
    String* matched = TRY(ToString(TRY(Get(result, PropertyKey::index(0)))));
    double matchLength = double(matched->length());
    double position = TRY(ToIntegerOrInfinity(TRY(Get(result, PropertyKey(u"index")))));
    position = std::min(std::max(position, 0.0), lengthS);
    ValueVector captures;
    for (double n = 1; n <= nCaptures; n++) {
      Value capN = TRY(Get(result, indexKey(n)));
      if (!capN.isUndefined()) capN = Value(TRY(ToString(capN)));
      captures.push_back(capN);
    }
    Value namedCaptures = TRY(Get(result, PropertyKey(u"groups")));
    String* replacement;
    if (functionalReplace) {
      ValueVector replacerArgs;
      replacerArgs.push_back(matched);
      for (Value c : captures) replacerArgs.push_back(c);
      replacerArgs.push_back(num(position));
      replacerArgs.push_back(s);
      if (!namedCaptures.isUndefined()) replacerArgs.push_back(namedCaptures);
      Value replValue = TRY(Call(replaceValue, Value::undefined(), replacerArgs));
      replacement = TRY(ToString(replValue));
    } else {
      if (!namedCaptures.isUndefined()) namedCaptures = Value(TRY(ToObject(namedCaptures)));
      replacement = TRY(GetSubstitution(matched, s, position, captures, namedCaptures, replaceTemplate));
    }
    if (position >= nextSourcePosition) {
      accumulated.append(s->view().substr(size_t(nextSourcePosition), size_t(position - nextSourcePosition)));
      accumulated.append(replacement->view());
      nextSourcePosition = position + matchLength;
    }
  }
  if (nextSourcePosition < lengthS) accumulated.append(s->view().substr(size_t(nextSourcePosition)));
  return Value(newString(std::move(accumulated)));
}

// 22.2.6.14 RegExp.prototype [ @@split ] ( string, limit )
Result<Value> RegExpPrototype_split(NativeCall& call) {
  Object* rx = TRY(thisObject(call.thisValue, "[Symbol.split]"));
  String* s = TRY(ToString(call.args[0]));
  Object* c = TRY(SpeciesConstructor(rx, intrinsic(Intrinsic::RegExp)));
  String* flags = TRY(flagsOfObject(rx));
  bool unicodeMatching = hasUnit(flags, u'u') || hasUnit(flags, u'v');
  String* newFlags = hasUnit(flags, u'y') ? flags : concatStrings({flags->view(), u"y"});
  Object* splitter = TRY(Construct(c, {Value(rx), Value(newFlags)}));
  Object* a = TRY(ArrayCreate(0));
  double lengthA = 0;
  uint32_t lim = call.args[1].isUndefined() ? 0xFFFFFFFFu : TRY(ToUint32(call.args[1]));
  if (lim == 0) return Value(a);
  double size = double(s->length());
  if (size == 0) {
    Value z = TRY(RegExpExec(splitter, s));
    if (!z.isNull()) return Value(a);
    TRY(CreateDataPropertyOrThrow(a, PropertyKey::index(0), Value(s)));
    return Value(a);
  }
  double p = 0, q = p;
  while (q < size) {
    TRY(Set(splitter, lastIndexKey(), num(q), true));
    Value z = TRY(RegExpExec(splitter, s));
    if (z.isNull()) {
      q = AdvanceStringIndex(s, q, unicodeMatching);
      continue;
    }
    double e = TRY(ToLength(TRY(Get(splitter, lastIndexKey()))));
    e = std::min(e, size);
    if (e == p) {
      q = AdvanceStringIndex(s, q, unicodeMatching);
      continue;
    }
    TRY(CreateDataPropertyOrThrow(a, indexKey(lengthA), Value(newString(s->view().substr(size_t(p), size_t(q - p))))));
    lengthA++;
    if (lengthA == lim) return Value(a);
    p = e;
    double numberOfCaptures = std::max(TRY(LengthOfArrayLike(z.asObject())) - 1, 0.0);
    for (double i = 1; i <= numberOfCaptures; i++) {
      Value nextCapture = TRY(Get(z.asObject(), indexKey(i)));
      TRY(CreateDataPropertyOrThrow(a, indexKey(lengthA), nextCapture));
      lengthA++;
      if (lengthA == lim) return Value(a);
    }
    q = p;
  }
  TRY(CreateDataPropertyOrThrow(a, indexKey(lengthA), Value(newString(s->view().substr(size_t(p))))));
  return Value(a);
}

// %RegExpStringIteratorPrototype% objects (22.2.9).
class RegExpStringIterator : public Object {
 public:
  RegExpStringIterator(Object* prototype, Object* matcher, String* s, bool global, bool fullUnicode)
      : Object(prototype), matcher(matcher), string(s), global(global), fullUnicode(fullUnicode) {}
  Object* matcher;  // nullptr once done
  String* string;
  bool global, fullUnicode;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(reinterpret_cast<Cell*>(matcher));
    tracer.visit(reinterpret_cast<Cell*>(string));
  }
};

// 22.2.6.9 RegExp.prototype [ @@matchAll ] ( string )
Result<Value> RegExpPrototype_matchAll(NativeCall& call) {
  Object* r = TRY(thisObject(call.thisValue, "[Symbol.matchAll]"));
  String* s = TRY(ToString(call.args[0]));
  Object* c = TRY(SpeciesConstructor(r, intrinsic(Intrinsic::RegExp)));
  String* flags = TRY(flagsOfObject(r));
  Object* matcher = TRY(Construct(c, {Value(r), Value(flags)}));
  double lastIndex = TRY(ToLength(TRY(Get(r, lastIndexKey()))));
  TRY(Set(matcher, lastIndexKey(), num(lastIndex), true));
  bool global = hasUnit(flags, u'g'), fullUnicode = hasUnit(flags, u'u') || hasUnit(flags, u'v');
  return Value(make<RegExpStringIterator>(intrinsic(Intrinsic::RegExpStringIteratorPrototype), matcher, s, global, fullUnicode));
}

// 22.2.9.2.1 %RegExpStringIteratorPrototype%.next ( )
Result<Value> RegExpStringIteratorPrototype_next(NativeCall& call) {
  auto* it = call.thisValue.isObject() ? call.thisValue.asObject()->as<RegExpStringIterator>() : nullptr;
  if (!it) return ThrowTypeError("next method called on incompatible receiver");
  if (!it->matcher) return Value(CreateIteratorResultObject(Value::undefined(), true));
  Object* r = it->matcher;
  Result<Value> matchResult = RegExpExec(r, it->string);
  if (matchResult.isThrow()) {
    it->matcher = nullptr;
    return Throw{matchResult.thrown()};
  }
  Value match = matchResult.value();
  if (match.isNull()) {
    it->matcher = nullptr;
    return Value(CreateIteratorResultObject(Value::undefined(), true));
  }
  if (!it->global) {
    it->matcher = nullptr;
    return Value(CreateIteratorResultObject(match, false));
  }
  Result<Value> first = Get(match.asObject(), PropertyKey::index(0));
  Result<String*> matchStr = first.isThrow() ? Result<String*>(Throw{first.thrown()}) : ToString(first.value());
  if (matchStr.isThrow()) {
    it->matcher = nullptr;
    return Throw{matchStr.thrown()};
  }
  if (matchStr.value()->length() == 0) {
    Result<Value> li = Get(r, lastIndexKey());
    Result<double> thisIndex = li.isThrow() ? Result<double>(Throw{li.thrown()}) : ToLength(li.value());
    if (thisIndex.isThrow()) {
      it->matcher = nullptr;
      return Throw{thisIndex.thrown()};
    }
    Result<void> set = Set(r, lastIndexKey(), num(AdvanceStringIndex(it->string, thisIndex.value(), it->fullUnicode)), true);
    if (set.isThrow()) {
      it->matcher = nullptr;
      return Throw{set.thrown()};
    }
  }
  return Value(CreateIteratorResultObject(match, false));
}

bool isRegExpObject(Object* o) { return o->is<RegExpObject>(); }

void installRegExp(Realm& realm) {
  setRegExpCheck(isRegExpObject);
  regExpCreate = RegExpCreate;
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor =
      defineConstructor(realm, Intrinsic::RegExp, RegExp_construct, 2, PropertyKey(u"RegExp"), proto, Intrinsic::RegExpPrototype);
  defineFunction(realm, ctor, PropertyKey(u"escape"), 1, RegExp_escape);
  defineAccessor(realm, ctor, PropertyKey(symbols().species), RegExp_species, nullptr);
  installLegacyRegExpStaticProperties(realm, ctor);

  defineFunction(realm, proto, PropertyKey(u"compile"), 2, RegExpPrototype_compile);
  defineAccessor(realm, proto, PropertyKey(u"dotAll"), RegExpHasFlag<u's'>, nullptr);
  defineFunction(realm, proto, PropertyKey(u"exec"), 1, RegExpPrototype_exec);
  defineAccessor(realm, proto, PropertyKey(u"flags"), RegExpPrototype_flags, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"global"), RegExpHasFlag<u'g'>, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"hasIndices"), RegExpHasFlag<u'd'>, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"ignoreCase"), RegExpHasFlag<u'i'>, nullptr);
  defineFunction(realm, proto, PropertyKey(symbols().match), 1, RegExpPrototype_match);
  defineFunction(realm, proto, PropertyKey(symbols().matchAll), 1, RegExpPrototype_matchAll);
  defineAccessor(realm, proto, PropertyKey(u"multiline"), RegExpHasFlag<u'm'>, nullptr);
  defineFunction(realm, proto, PropertyKey(symbols().replace), 2, RegExpPrototype_replace);
  defineFunction(realm, proto, PropertyKey(symbols().search), 1, RegExpPrototype_search);
  defineAccessor(realm, proto, PropertyKey(u"source"), RegExpPrototype_source, nullptr);
  defineFunction(realm, proto, PropertyKey(symbols().split), 2, RegExpPrototype_split);
  defineAccessor(realm, proto, PropertyKey(u"sticky"), RegExpHasFlag<u'y'>, nullptr);
  defineFunction(realm, proto, PropertyKey(u"test"), 1, RegExpPrototype_test);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, RegExpPrototype_toString);
  defineAccessor(realm, proto, PropertyKey(u"unicode"), RegExpHasFlag<u'u'>, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"unicodeSets"), RegExpHasFlag<u'v'>, nullptr);

  Object* iterProto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::IteratorPrototype));
  realm.setIntrinsic(Intrinsic::RegExpStringIteratorPrototype, iterProto);
  defineFunction(realm, iterProto, PropertyKey(u"next"), 0, RegExpStringIteratorPrototype_next);
  defineValue(iterProto, PropertyKey(symbols().toStringTag), str(u"RegExp String Iterator"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(RegExp, kOrderDefault, installRegExp);

}  // namespace snowman::builtins
