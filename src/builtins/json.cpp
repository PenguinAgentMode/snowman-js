// The JSON object (25.5).
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "builtins/util.h"
#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/wrappers.h"

namespace snowman::builtins {

namespace {

// A recursive-descent parser for the JSON grammar (ECMA-404), building values as JSON.parse's
// evaluation of the text as an ECMAScript literal would.
class JSONParser {
 public:
  explicit JSONParser(std::u16string_view text) : s_(text) {}

  Result<Value> parseText() {
    Value v = TRY(parseValue());
    skipWhitespace();
    if (pos_ != s_.size()) return error();
    return v;
  }

 private:
  Throw error() { return ThrowSyntaxError("JSON.parse: unexpected character at position " + std::to_string(pos_)); }

  void skipWhitespace() {
    while (pos_ < s_.size() && (s_[pos_] == u' ' || s_[pos_] == u'\t' || s_[pos_] == u'\n' || s_[pos_] == u'\r')) pos_++;
  }

  bool consume(std::u16string_view word) {
    if (s_.substr(pos_, word.size()) != word) return false;
    pos_ += word.size();
    return true;
  }

  Result<Value> parseValue() {
    TRY(CheckStack());
    skipWhitespace();
    if (pos_ >= s_.size()) return error();
    char16_t c = s_[pos_];
    if (c == u'{') return parseObject();
    if (c == u'[') return parseArray();
    if (c == u'"') return Value(TRY(parseString()));
    if (c == u'-' || (c >= u'0' && c <= u'9')) return parseNumber();
    if (consume(u"true")) return Value::boolean(true);
    if (consume(u"false")) return Value::boolean(false);
    if (consume(u"null")) return Value::null();
    return error();
  }

  Result<Value> parseNumber() {
    size_t start = pos_;
    if (s_[pos_] == u'-') pos_++;
    if (pos_ >= s_.size()) return error();
    if (s_[pos_] == u'0') {
      pos_++;
    } else if (s_[pos_] >= u'1' && s_[pos_] <= u'9') {
      while (pos_ < s_.size() && s_[pos_] >= u'0' && s_[pos_] <= u'9') pos_++;
    } else {
      return error();
    }
    if (pos_ < s_.size() && s_[pos_] == u'.') {
      pos_++;
      size_t digits = pos_;
      while (pos_ < s_.size() && s_[pos_] >= u'0' && s_[pos_] <= u'9') pos_++;
      if (pos_ == digits) return error();
    }
    if (pos_ < s_.size() && (s_[pos_] == u'e' || s_[pos_] == u'E')) {
      pos_++;
      if (pos_ < s_.size() && (s_[pos_] == u'+' || s_[pos_] == u'-')) pos_++;
      size_t digits = pos_;
      while (pos_ < s_.size() && s_[pos_] >= u'0' && s_[pos_] <= u'9') pos_++;
      if (pos_ == digits) return error();
    }
    std::string ascii(s_.begin() + start, s_.begin() + pos_);
    return num(std::strtod(ascii.c_str(), nullptr));
  }

  static int hex(char16_t c) {
    if (c >= u'0' && c <= u'9') return c - u'0';
    if (c >= u'a' && c <= u'f') return c - u'a' + 10;
    if (c >= u'A' && c <= u'F') return c - u'A' + 10;
    return -1;
  }

  Result<String*> parseString() {
    pos_++;  // "
    std::u16string out;
    while (true) {
      if (pos_ >= s_.size()) return error();
      char16_t c = s_[pos_++];
      if (c == u'"') break;
      if (c < 0x20) {
        pos_--;
        return error();
      }
      if (c != u'\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= s_.size()) return error();
      char16_t e = s_[pos_++];
      switch (e) {
        case u'"': out.push_back(u'"'); break;
        case u'\\': out.push_back(u'\\'); break;
        case u'/': out.push_back(u'/'); break;
        case u'b': out.push_back(u'\b'); break;
        case u'f': out.push_back(u'\f'); break;
        case u'n': out.push_back(u'\n'); break;
        case u'r': out.push_back(u'\r'); break;
        case u't': out.push_back(u'\t'); break;
        case u'u': {
          if (pos_ + 4 > s_.size()) return error();
          int v = 0;
          for (int i = 0; i < 4; i++) {
            int h = hex(s_[pos_ + size_t(i)]);
            if (h < 0) return error();
            v = v * 16 + h;
          }
          pos_ += 4;
          out.push_back(char16_t(v));
          break;
        }
        default:
          pos_--;
          return error();
      }
    }
    return newString(std::move(out));
  }

  Result<Value> parseArray() {
    pos_++;  // [
    ValueVector elements;
    skipWhitespace();
    if (pos_ < s_.size() && s_[pos_] == u']') {
      pos_++;
      return Value(CreateArrayFromList(elements));
    }
    while (true) {
      elements.push_back(TRY(parseValue()));
      skipWhitespace();
      if (pos_ >= s_.size()) return error();
      if (s_[pos_] == u',') {
        pos_++;
        continue;
      }
      if (s_[pos_] == u']') {
        pos_++;
        return Value(CreateArrayFromList(elements));
      }
      return error();
    }
  }

  Result<Value> parseObject() {
    pos_++;  // {
    Object* obj = newPlainObject();
    skipWhitespace();
    if (pos_ < s_.size() && s_[pos_] == u'}') {
      pos_++;
      return Value(obj);
    }
    while (true) {
      skipWhitespace();
      if (pos_ >= s_.size() || s_[pos_] != u'"') return error();
      String* key = TRY(parseString());
      skipWhitespace();
      if (pos_ >= s_.size() || s_[pos_] != u':') return error();
      pos_++;
      Value v = TRY(parseValue());
      TRY(CreateDataProperty(obj, PropertyKey(key), v));
      skipWhitespace();
      if (pos_ >= s_.size()) return error();
      if (s_[pos_] == u',') {
        pos_++;
        continue;
      }
      if (s_[pos_] == u'}') {
        pos_++;
        return Value(obj);
      }
      return error();
    }
  }

  std::u16string_view s_;
  size_t pos_ = 0;
};

// 25.5.1.1 InternalizeJSONProperty ( holder, name, reviver )
Result<Value> InternalizeJSONProperty(Object* holder, const PropertyKey& name, Value reviver) {
  TRY(CheckStack());
  Value val = TRY(Get(holder, name));
  if (val.isObject()) {
    Object* o = val.asObject();
    bool isArray = TRY(IsArray(val));
    if (isArray) {
      double len = TRY(LengthOfArrayLike(o));
      for (double i = 0; i < len; i++) {
        PropertyKey prop = indexKey(i);
        Value newElement = TRY(InternalizeJSONProperty(o, prop, reviver));
        if (newElement.isUndefined()) TRY(o->Delete(prop));
        else TRY(CreateDataProperty(o, prop, newElement));
      }
    } else {
      ValueVector keys = TRY(EnumerableOwnProperties(o, EnumerableOwnKind::Key));
      for (Value p : keys) {
        PropertyKey prop(p.asString());
        Value newElement = TRY(InternalizeJSONProperty(o, prop, reviver));
        if (newElement.isUndefined()) TRY(o->Delete(prop));
        else TRY(CreateDataProperty(o, prop, newElement));
      }
    }
  }
  return Call(reviver, holder, {name.toValue(), val});
}

// 25.5.1 JSON.parse ( text [ , reviver ] )
Result<Value> JSON_parse(NativeCall& call) {
  String* jsonString = TRY(ToString(call.args[0]));
  JSONParser parser(jsonString->view());
  Value unfiltered = TRY(parser.parseText());
  Value reviver = call.args[1];
  if (IsCallable(reviver)) {
    Object* root = newPlainObject();
    PropertyKey rootName(u"");
    TRY(CreateDataPropertyOrThrow(root, rootName, unfiltered));
    return InternalizeJSONProperty(root, rootName, reviver);
  }
  return unfiltered;
}

// 25.5.2.3 QuoteJSONString ( value )
void QuoteJSONString(std::u16string& out, std::u16string_view value) {
  static const char hex[] = "0123456789abcdef";
  out.push_back(u'"');
  for (size_t i = 0; i < value.size(); i++) {
    char16_t c = value[i];
    switch (c) {
      case u'\b': out.append(u"\\b"); continue;
      case u'\t': out.append(u"\\t"); continue;
      case u'\n': out.append(u"\\n"); continue;
      case u'\f': out.append(u"\\f"); continue;
      case u'\r': out.append(u"\\r"); continue;
      case u'"': out.append(u"\\\""); continue;
      case u'\\': out.append(u"\\\\"); continue;
      default: break;
    }
    bool lone = false;
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 < value.size() && value[i + 1] >= 0xDC00 && value[i + 1] <= 0xDFFF) {
        out.push_back(c);
        out.push_back(value[++i]);
        continue;
      }
      lone = true;
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      lone = true;
    }
    if (c < 0x20 || lone) {
      out.append(u"\\u");
      out.push_back(char16_t(hex[(c >> 12) & 15]));
      out.push_back(char16_t(hex[(c >> 8) & 15]));
      out.push_back(char16_t(hex[(c >> 4) & 15]));
      out.push_back(char16_t(hex[c & 15]));
    } else {
      out.push_back(c);
    }
  }
  out.push_back(u'"');
}

// 25.5.2.1 JSON Serialization Record
struct SerializationState {
  Value replacerFunction;
  ValueVector stack;
  std::u16string indent;
  std::u16string gap;
  std::optional<std::vector<PropertyKey>> propertyList;
};

Result<std::optional<std::u16string>> SerializeJSONProperty(SerializationState& state, const PropertyKey& key, Object* holder);

Result<void> checkCycle(SerializationState& state, Object* value) {
  for (Value v : state.stack) {
    if (v.asObject() == value) return ThrowTypeError("Converting circular structure to JSON");
  }
  state.stack.push_back(value);
  return {};
}

std::u16string joinPartial(SerializationState& state, const std::vector<std::u16string>& partial, char16_t open,
                           char16_t close, const std::u16string& stepback) {
  std::u16string final;
  final.push_back(open);
  if (partial.empty()) {
    final.push_back(close);
    return final;
  }
  if (state.gap.empty()) {
    for (size_t i = 0; i < partial.size(); i++) {
      if (i) final.push_back(u',');
      final.append(partial[i]);
    }
  } else {
    final.push_back(u'\n');
    final.append(state.indent);
    for (size_t i = 0; i < partial.size(); i++) {
      if (i) {
        final.append(u",\n");
        final.append(state.indent);
      }
      final.append(partial[i]);
    }
    final.push_back(u'\n');
    final.append(stepback);
  }
  final.push_back(close);
  return final;
}

// 25.5.2.5 SerializeJSONObject ( state, value )
Result<std::u16string> SerializeJSONObject(SerializationState& state, Object* value) {
  TRY(checkCycle(state, value));
  std::u16string stepback = state.indent;
  state.indent += state.gap;
  std::vector<PropertyKey> k;
  if (state.propertyList) {
    k = *state.propertyList;
  } else {
    ValueVector keys = TRY(EnumerableOwnProperties(value, EnumerableOwnKind::Key));
    for (Value v : keys) k.push_back(PropertyKey(v.asString()));
  }
  std::vector<std::u16string> partial;
  for (const PropertyKey& p : k) {
    std::optional<std::u16string> strP = TRY(SerializeJSONProperty(state, p, value));
    if (!strP) continue;
    std::u16string member;
    QuoteJSONString(member, p.toValue().asString()->view());
    member.push_back(u':');
    if (!state.gap.empty()) member.push_back(u' ');
    member.append(*strP);
    partial.push_back(std::move(member));
  }
  std::u16string final = joinPartial(state, partial, u'{', u'}', stepback);
  state.stack.pop_back();
  state.indent = stepback;
  return final;
}

// 25.5.2.6 SerializeJSONArray ( state, value )
Result<std::u16string> SerializeJSONArray(SerializationState& state, Object* value) {
  TRY(checkCycle(state, value));
  std::u16string stepback = state.indent;
  state.indent += state.gap;
  std::vector<std::u16string> partial;
  double len = TRY(LengthOfArrayLike(value));
  for (double index = 0; index < len; index++) {
    std::optional<std::u16string> strP = TRY(SerializeJSONProperty(state, indexKey(index), value));
    partial.push_back(strP ? *strP : std::u16string(u"null"));
  }
  std::u16string final = joinPartial(state, partial, u'[', u']', stepback);
  state.stack.pop_back();
  state.indent = stepback;
  return final;
}

// 25.5.2.2 SerializeJSONProperty ( state, key, holder )
Result<std::optional<std::u16string>> SerializeJSONProperty(SerializationState& state, const PropertyKey& key, Object* holder) {
  TRY(CheckStack());
  Value value = TRY(Get(holder, key));
  if (value.isObject() || value.isBigInt()) {
    Value toJSON = TRY(GetV(value, PropertyKey(u"toJSON")));
    if (IsCallable(toJSON)) value = TRY(Call(toJSON, value, {key.toValue()}));
  }
  if (!state.replacerFunction.isUndefined()) value = TRY(Call(state.replacerFunction, holder, {key.toValue(), value}));
  if (value.isObject()) {
    Object* o = value.asObject();
    if (o->is<NumberObject>()) value = num(TRY(ToNumber(value)));
    else if (o->is<StringObject>()) value = Value(TRY(ToString(value)));
    else if (auto* b = o->as<BooleanObject>()) value = Value::boolean(b->booleanData);
    else if (auto* bi = o->as<BigIntObject>()) value = Value(bi->bigintData);
  }
  using R = std::optional<std::u16string>;
  if (value.isNull()) return R(u"null");
  if (value.isBoolean()) return R(value.asBoolean() ? u"true" : u"false");
  if (value.isString()) {
    std::u16string out;
    QuoteJSONString(out, value.asString()->view());
    return R(std::move(out));
  }
  if (value.isNumber()) {
    if (!std::isfinite(value.asNumber())) return R(u"null");
    return R(std::u16string(NumberToString(value.asNumber())->view()));
  }
  if (value.isBigInt()) return ThrowTypeError("Do not know how to serialize a BigInt");
  if (value.isObject() && !IsCallable(value)) {
    bool isArray = TRY(IsArray(value));
    if (isArray) return R(TRY(SerializeJSONArray(state, value.asObject())));
    return R(TRY(SerializeJSONObject(state, value.asObject())));
  }
  return R();
}

// 25.5.2 JSON.stringify ( value [ , replacer [ , space ] ] )
Result<Value> JSON_stringify(NativeCall& call) {
  SerializationState state;
  Value replacer = call.args[1], space = call.args[2];
  if (replacer.isObject()) {
    if (IsCallable(replacer)) {
      state.replacerFunction = replacer;
    } else {
      bool isArray = TRY(IsArray(replacer));
      if (isArray) {
        std::vector<PropertyKey> list;
        double len = TRY(LengthOfArrayLike(replacer.asObject()));
        for (double k = 0; k < len; k++) {
          Value v = TRY(Get(replacer.asObject(), indexKey(k)));
          std::optional<PropertyKey> item;
          if (v.isString()) {
            item = PropertyKey(v.asString());
          } else if (v.isNumber()) {
            item = PropertyKey(NumberToString(v.asNumber()));
          } else if (v.isObject() && (v.asObject()->is<StringObject>() || v.asObject()->is<NumberObject>())) {
            item = PropertyKey(TRY(ToString(v)));
          }
          if (item && std::find(list.begin(), list.end(), *item) == list.end()) list.push_back(*item);
        }
        state.propertyList = std::move(list);
      }
    }
  }
  if (space.isObject()) {
    if (space.asObject()->is<NumberObject>()) space = num(TRY(ToNumber(space)));
    else if (space.asObject()->is<StringObject>()) space = Value(TRY(ToString(space)));
  }
  if (space.isNumber()) {
    double spaceMV = std::min(10.0, TRY(ToIntegerOrInfinity(space)));
    if (spaceMV >= 1) state.gap = std::u16string(size_t(spaceMV), u' ');
  } else if (space.isString()) {
    state.gap = std::u16string(space.asString()->view().substr(0, 10));
  }
  Object* wrapper = newPlainObject();
  TRY(CreateDataPropertyOrThrow(wrapper, PropertyKey(u""), call.args[0]));
  std::optional<std::u16string> result = TRY(SerializeJSONProperty(state, PropertyKey(u""), wrapper));
  if (!result) return Value::undefined();
  return Value(newString(std::move(*result)));
}

void installJSON(Realm& realm) {
  Object* json = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  realm.setIntrinsic(Intrinsic::JSON, json);
  defineGlobal(realm, PropertyKey(u"JSON"), json);
  defineFunction(realm, json, PropertyKey(u"parse"), 2, JSON_parse);
  defineFunction(realm, json, PropertyKey(u"stringify"), 3, JSON_stringify);
  defineValue(json, PropertyKey(vm().symbols.toStringTag), str(u"JSON"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(JSON, kOrderDefault, installJSON);

}  // namespace snowman::builtins
