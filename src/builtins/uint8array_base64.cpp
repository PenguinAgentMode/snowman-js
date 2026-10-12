// Uint8Array to/from base64 and hex (Uint8Array.fromBase64/fromHex, prototype.toBase64/toHex/setFromBase64/
// setFromHex).
#include <cstring>

#include "builtins/util.h"
#include "runtime/function.h"
#include "runtime/typedarray.h"

namespace snowman::builtins {

namespace {

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(char16_t c) {
  if (c >= u'A' && c <= u'Z') return c - u'A';
  if (c >= u'a' && c <= u'z') return c - u'a' + 26;
  if (c >= u'0' && c <= u'9') return c - u'0' + 52;
  if (c == u'+') return 62;
  if (c == u'/') return 63;
  return -1;
}

int hexValue(char16_t c) {
  if (c >= u'0' && c <= u'9') return c - u'0';
  if (c >= u'a' && c <= u'f') return c - u'a' + 10;
  if (c >= u'A' && c <= u'F') return c - u'A' + 10;
  return -1;
}

struct DecodeResult {
  double read = 0;
  std::vector<uint8_t> bytes;
  bool error = false;
};

size_t SkipAsciiWhitespace(std::u16string_view s, size_t index) {
  while (index < s.size() && (s[index] == 0x09 || s[index] == 0x0A || s[index] == 0x0C || s[index] == 0x0D || s[index] == 0x20)) index++;
  return index;
}

// DecodeBase64Chunk ( chunk [ , throwOnExtraBits ] ): false on a SyntaxError.
bool DecodeBase64Chunk(std::u16string chunk, bool throwOnExtraBits, std::vector<uint8_t>& out) {
  size_t chunkLength = chunk.size();
  while (chunk.size() < 4) chunk.push_back(u'A');
  uint32_t v = 0;
  for (char16_t c : chunk) v = (v << 6) | uint32_t(base64Value(c));
  uint8_t b[3] = {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
  if (chunkLength == 2) {
    if (throwOnExtraBits && b[1] != 0) return false;
    out.push_back(b[0]);
  } else if (chunkLength == 3) {
    if (throwOnExtraBits && b[2] != 0) return false;
    out.push_back(b[0]);
    out.push_back(b[1]);
  } else {
    out.insert(out.end(), b, b + 3);
  }
  return true;
}

enum class LastChunk { Loose, Strict, StopBeforePartial };

// FromBase64 ( string, alphabet, lastChunkHandling [ , maxLength ] )
DecodeResult FromBase64(std::u16string_view s, bool url, LastChunk lastChunkHandling, double maxLength) {
  DecodeResult r;
  if (maxLength == 0) return r;
  std::u16string chunk;
  size_t index = 0, length = s.size();
  auto fail = [&]() {
    r.error = true;
    return r;
  };
  while (true) {
    index = SkipAsciiWhitespace(s, index);
    if (index == length) {
      if (!chunk.empty()) {
        if (lastChunkHandling == LastChunk::StopBeforePartial) return r;
        if (lastChunkHandling == LastChunk::Loose) {
          if (chunk.size() == 1) return fail();
          DecodeBase64Chunk(chunk, false, r.bytes);
        } else {
          return fail();
        }
      }
      r.read = double(length);
      return r;
    }
    char16_t c = s[index++];
    if (c == u'=') {
      if (chunk.size() < 2) return fail();
      index = SkipAsciiWhitespace(s, index);
      if (chunk.size() == 2) {
        if (index == length) {
          if (lastChunkHandling == LastChunk::StopBeforePartial) return r;
          return fail();
        }
        if (s[index] == u'=') index = SkipAsciiWhitespace(s, index + 1);
      }
      if (index < length) return fail();
      if (!DecodeBase64Chunk(chunk, lastChunkHandling == LastChunk::Strict, r.bytes)) return fail();
      r.read = double(length);
      return r;
    }
    if (url) {
      if (c == u'+' || c == u'/') return fail();
      if (c == u'-') c = u'+';
      else if (c == u'_') c = u'/';
    }
    if (base64Value(c) < 0) return fail();
    double remaining = maxLength - double(r.bytes.size());
    if ((remaining == 1 && chunk.size() == 2) || (remaining == 2 && chunk.size() == 3)) return r;
    chunk.push_back(c);
    if (chunk.size() == 4) {
      DecodeBase64Chunk(chunk, false, r.bytes);
      chunk.clear();
      r.read = double(index);
      if (double(r.bytes.size()) == maxLength) return r;
    }
  }
}

// FromHex ( string [ , maxLength ] )
DecodeResult FromHex(std::u16string_view s, double maxLength) {
  DecodeResult r;
  size_t length = s.size(), read = 0;
  if (length % 2 != 0) {
    r.error = true;
    return r;
  }
  while (read < length && double(r.bytes.size()) < maxLength) {
    int hi = hexValue(s[read]), lo = hexValue(s[read + 1]);
    if (hi < 0 || lo < 0) {
      r.error = true;
      r.read = double(read);
      return r;
    }
    read += 2;
    r.bytes.push_back(uint8_t(hi * 16 + lo));
  }
  r.read = double(read);
  return r;
}

// GetOptionsObject ( options )
Result<Object*> GetOptionsObject(Value options) {
  if (options.isUndefined()) return OrdinaryObjectCreate(nullptr);
  if (options.isObject()) return options.asObject();
  return ThrowTypeError("options must be an object");
}

Result<bool> alphabetOption(Object* opts) {
  Value alphabet = TRY(Get(opts, PropertyKey(u"alphabet")));
  if (alphabet.isUndefined()) return false;
  if (alphabet.isString() && alphabet.asString()->view() == u"base64") return false;
  if (alphabet.isString() && alphabet.asString()->view() == u"base64url") return true;
  return ThrowTypeError("alphabet must be \"base64\" or \"base64url\"");
}

Result<LastChunk> lastChunkOption(Object* opts) {
  Value v = TRY(Get(opts, PropertyKey(u"lastChunkHandling")));
  if (v.isUndefined()) return LastChunk::Loose;
  if (v.isString()) {
    std::u16string_view s = v.asString()->view();
    if (s == u"loose") return LastChunk::Loose;
    if (s == u"strict") return LastChunk::Strict;
    if (s == u"stop-before-partial") return LastChunk::StopBeforePartial;
  }
  return ThrowTypeError("lastChunkHandling must be \"loose\", \"strict\" or \"stop-before-partial\"");
}

Result<TypedArrayObject*> ValidateUint8Array(Value v) {
  TypedArrayObject* ta = v.isObject() ? v.asObject()->as<TypedArrayObject>() : nullptr;
  if (!ta || ta->elementType != ElementType::Uint8) return ThrowTypeError("this is not a Uint8Array");
  return ta;
}

Result<Value> newUint8Array(const std::vector<uint8_t>& bytes) {
  Object* ctor = intrinsic(Intrinsic::Uint8Array);
  Object* ta = TRY(Construct(ctor, {num(double(bytes.size()))}));
  TypedArrayObject* u8 = ta->as<TypedArrayObject>();
  if (!bytes.empty()) std::memcpy(u8->buffer->data() + size_t(u8->byteOffset), bytes.data(), bytes.size());
  return Value(ta);
}

// Uint8Array.fromBase64 ( string [ , options ] )
Result<Value> Uint8Array_fromBase64(NativeCall& call) {
  if (!call.args[0].isString()) return ThrowTypeError("Uint8Array.fromBase64 requires a string");
  Object* opts = TRY(GetOptionsObject(call.args[1]));
  bool url = TRY(alphabetOption(opts));
  LastChunk last = TRY(lastChunkOption(opts));
  DecodeResult r = FromBase64(call.args[0].asString()->view(), url, last, 9007199254740991.0);
  if (r.error) return ThrowSyntaxError("invalid base64 string");
  return newUint8Array(r.bytes);
}

// Uint8Array.fromHex ( string )
Result<Value> Uint8Array_fromHex(NativeCall& call) {
  if (!call.args[0].isString()) return ThrowTypeError("Uint8Array.fromHex requires a string");
  DecodeResult r = FromHex(call.args[0].asString()->view(), 9007199254740991.0);
  if (r.error) return ThrowSyntaxError("invalid hex string");
  return newUint8Array(r.bytes);
}

Result<Value> setFromResult(TypedArrayObject* into, const DecodeResult& r, const char* what) {
  if (!r.bytes.empty()) std::memcpy(into->buffer->data() + size_t(into->byteOffset), r.bytes.data(), r.bytes.size());
  if (r.error) return ThrowSyntaxError(std::string("invalid ") + what + " string");
  Object* result = newPlainObject();
  TRY(CreateDataPropertyOrThrow(result, PropertyKey(u"read"), num(r.read)));
  TRY(CreateDataPropertyOrThrow(result, PropertyKey(u"written"), num(double(r.bytes.size()))));
  return Value(result);
}

// Uint8Array.prototype.setFromBase64 ( string [ , options ] )
Result<Value> Uint8ArrayPrototype_setFromBase64(NativeCall& call) {
  TypedArrayObject* into = TRY(ValidateUint8Array(call.thisValue));
  if (!call.args[0].isString()) return ThrowTypeError("setFromBase64 requires a string");
  Object* opts = TRY(GetOptionsObject(call.args[1]));
  bool url = TRY(alphabetOption(opts));
  LastChunk last = TRY(lastChunkOption(opts));
  if (IsTypedArrayOutOfBounds(into)) return ThrowTypeError("typed array is detached or out of bounds");
  DecodeResult r = FromBase64(call.args[0].asString()->view(), url, last, TypedArrayLength(into));
  return setFromResult(into, r, "base64");
}

// Uint8Array.prototype.setFromHex ( string )
Result<Value> Uint8ArrayPrototype_setFromHex(NativeCall& call) {
  TypedArrayObject* into = TRY(ValidateUint8Array(call.thisValue));
  if (!call.args[0].isString()) return ThrowTypeError("setFromHex requires a string");
  if (IsTypedArrayOutOfBounds(into)) return ThrowTypeError("typed array is detached or out of bounds");
  DecodeResult r = FromHex(call.args[0].asString()->view(), TypedArrayLength(into));
  return setFromResult(into, r, "hex");
}

// GetUint8ArrayBytes ( ta )
Result<std::vector<uint8_t>> GetUint8ArrayBytes(TypedArrayObject* ta) {
  if (IsTypedArrayOutOfBounds(ta)) return ThrowTypeError("typed array is detached or out of bounds");
  const uint8_t* start = ta->buffer->data() + size_t(ta->byteOffset);
  return std::vector<uint8_t>(start, start + size_t(TypedArrayLength(ta)));
}

// Uint8Array.prototype.toBase64 ( [ options ] )
Result<Value> Uint8ArrayPrototype_toBase64(NativeCall& call) {
  TypedArrayObject* o = TRY(ValidateUint8Array(call.thisValue));
  Object* opts = TRY(GetOptionsObject(call.args[0]));
  bool url = TRY(alphabetOption(opts));
  bool omitPadding = ToBoolean(TRY(Get(opts, PropertyKey(u"omitPadding"))));
  std::vector<uint8_t> bytes = TRY(GetUint8ArrayBytes(o));
  std::u16string out;
  auto put = [&](uint32_t v) {
    char c = kBase64[v];
    if (url && c == '+') c = '-';
    if (url && c == '/') c = '_';
    out.push_back(char16_t(c));
  };
  size_t i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) | bytes[i + 2];
    put(v >> 18), put((v >> 12) & 63), put((v >> 6) & 63), put(v & 63);
  }
  size_t rest = bytes.size() - i;
  if (rest == 1) {
    uint32_t v = uint32_t(bytes[i]) << 16;
    put(v >> 18), put((v >> 12) & 63);
    if (!omitPadding) out.append(u"==");
  } else if (rest == 2) {
    uint32_t v = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8);
    put(v >> 18), put((v >> 12) & 63), put((v >> 6) & 63);
    if (!omitPadding) out.push_back(u'=');
  }
  return Value(newString(std::move(out)));
}

// Uint8Array.prototype.toHex ( )
Result<Value> Uint8ArrayPrototype_toHex(NativeCall& call) {
  TypedArrayObject* o = TRY(ValidateUint8Array(call.thisValue));
  std::vector<uint8_t> bytes = TRY(GetUint8ArrayBytes(o));
  static const char hex[] = "0123456789abcdef";
  std::u16string out;
  for (uint8_t b : bytes) {
    out.push_back(char16_t(hex[b >> 4]));
    out.push_back(char16_t(hex[b & 15]));
  }
  return Value(newString(std::move(out)));
}

void installUint8ArrayBase64(Realm& realm) {
  Object* ctor = realm.intrinsic(Intrinsic::Uint8Array);
  Object* proto = realm.intrinsic(Intrinsic::Uint8ArrayPrototype);
  defineFunction(realm, ctor, PropertyKey(u"fromBase64"), 1, Uint8Array_fromBase64);
  defineFunction(realm, ctor, PropertyKey(u"fromHex"), 1, Uint8Array_fromHex);
  defineFunction(realm, proto, PropertyKey(u"setFromBase64"), 1, Uint8ArrayPrototype_setFromBase64);
  defineFunction(realm, proto, PropertyKey(u"setFromHex"), 1, Uint8ArrayPrototype_setFromHex);
  defineFunction(realm, proto, PropertyKey(u"toBase64"), 0, Uint8ArrayPrototype_toBase64);
  defineFunction(realm, proto, PropertyKey(u"toHex"), 0, Uint8ArrayPrototype_toHex);
}

}  // namespace

SNOWMAN_BUILTIN(Uint8ArrayBase64, kOrderLate + 1, installUint8ArrayBase64);

}  // namespace snowman::builtins
