// %TypedArray% (23.2), its prototype, and the 12 TypedArray constructors.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "builtins/util.h"
#include "runtime/arraybuffer.h"
#include "runtime/bigint.h"
#include "runtime/function.h"
#include "runtime/typedarray.h"

namespace snowman::builtins {

Object* CreateArrayIteratorForKind(Object* array, int kind);
extern Result<double> (*arrayIteratorLength)(Object* array);

namespace {

constexpr ElementType kAllTypes[] = {ElementType::Int8,    ElementType::Uint8,    ElementType::Uint8C,
                                     ElementType::Int16,   ElementType::Uint16,   ElementType::Int32,
                                     ElementType::Uint32,  ElementType::Float16,  ElementType::Float32,
                                     ElementType::Float64, ElementType::BigInt64, ElementType::BigUint64};

bool sameContentType(ElementType a, ElementType b) { return IsBigIntElementType(a) == IsBigIntElementType(b); }

// The array iterator's length for TypedArrays (23.1.5.1 step 2.b.i).
Result<double> typedArrayAwareLength(Object* array) {
  if (auto* ta = array->as<TypedArrayObject>()) {
    if (IsTypedArrayOutOfBounds(ta)) return ThrowTypeError("typed array is detached or out of bounds");
    return TypedArrayLength(ta);
  }
  return LengthOfArrayLike(array);
}

// 23.2.5.1.6 AllocateTypedArrayBuffer ( O, length )
Result<void> AllocateTypedArrayBuffer(TypedArrayObject* o, double length) {
  double elementSize = double(ElementSize(o->elementType));
  ArrayBufferObject* data = TRY(AllocateArrayBuffer(intrinsic(Intrinsic::ArrayBuffer), elementSize * length));
  o->buffer = data;
  o->byteLength = elementSize * length;
  o->byteOffset = 0;
  o->arrayLength = length;
  return {};
}

// 23.2.5.1.1 AllocateTypedArray ( constructorName, newTarget, defaultProto [ , length ] )
Result<TypedArrayObject*> AllocateTypedArray(ElementType type, Object* newTarget, std::optional<double> length) {
  Object* proto = TRY(GetPrototypeFromConstructor(newTarget, TypedArrayPrototypeIntrinsic(type)));
  TypedArrayObject* obj = make<TypedArrayObject>(proto, type);
  if (length) TRY(AllocateTypedArrayBuffer(obj, *length));
  return obj;
}

// 23.2.4.2 TypedArrayCreateFromConstructor ( constructor, argumentList [ , accessMode ] )
Result<TypedArrayObject*> TypedArrayCreateFromConstructor(Object* constructor, Args argumentList,
                                                          AccessMode accessMode = AccessMode::Read) {
  Object* newObject = TRY(Construct(constructor, argumentList));
  TypedArrayObject* ta = TRY(ValidateTypedArray(newObject, accessMode));
  if (argumentList.size() == 1 && argumentList[0].isNumber()) {
    if (TypedArrayLength(ta) < argumentList[0].asNumber()) return ThrowTypeError("TypedArray species created a too short array");
  }
  return ta;
}

// 23.2.4.3 TypedArrayCreateSameType ( exemplar, argumentList )
Result<TypedArrayObject*> TypedArrayCreateSameType(TypedArrayObject* exemplar, Args argumentList) {
  return TypedArrayCreateFromConstructor(intrinsic(TypedArrayConstructorIntrinsic(exemplar->elementType)), argumentList);
}

// 23.2.4.1 TypedArraySpeciesCreate ( exemplar, argumentList [ , accessMode ] )
Result<TypedArrayObject*> TypedArraySpeciesCreate(TypedArrayObject* exemplar, Args argumentList,
                                                  AccessMode accessMode = AccessMode::Read) {
  Object* defaultConstructor = intrinsic(TypedArrayConstructorIntrinsic(exemplar->elementType));
  Object* constructor = TRY(SpeciesConstructor(exemplar, defaultConstructor));
  TypedArrayObject* result = TRY(TypedArrayCreateFromConstructor(constructor, argumentList, accessMode));
  if (!sameContentType(result->elementType, exemplar->elementType))
    return ThrowTypeError("TypedArray species created an array of a different content type");
  return result;
}

// 23.2.5.1.2 InitializeTypedArrayFromTypedArray ( O, srcArray )
Result<void> InitializeTypedArrayFromTypedArray(TypedArrayObject* o, TypedArrayObject* src) {
  if (IsTypedArrayOutOfBounds(src)) return ThrowTypeError("source typed array is detached or out of bounds");
  double elementLength = TypedArrayLength(src);
  size_t elementSize = ElementSize(o->elementType), srcElementSize = ElementSize(src->elementType);
  double byteLength = double(elementSize) * elementLength;
  if (!sameContentType(o->elementType, src->elementType)) return ThrowTypeError("Content types differ");
  ArrayBufferObject* data = TRY(AllocateArrayBuffer(intrinsic(Intrinsic::ArrayBuffer), byteLength));
  if (src->elementType == o->elementType) {
    std::memcpy(data->data(), src->buffer->data() + size_t(src->byteOffset), size_t(byteLength));
  } else {
    for (size_t i = 0; i < size_t(elementLength); i++) {
      Value v = GetValueFromBuffer(src->buffer, size_t(src->byteOffset) + i * srcElementSize, src->elementType);
      SetValueInBuffer(data, i * elementSize, o->elementType, v);
    }
  }
  o->buffer = data;
  o->byteLength = byteLength;
  o->byteOffset = 0;
  o->arrayLength = elementLength;
  return {};
}

// 23.2.5.1.3 InitializeTypedArrayFromArrayBuffer ( O, buffer, byteOffset, length )
Result<void> InitializeTypedArrayFromArrayBuffer(TypedArrayObject* o, ArrayBufferObject* buffer, Value byteOffset, Value length) {
  double elementSize = double(ElementSize(o->elementType));
  double offset = TRY(ToIndex(byteOffset));
  if (std::fmod(offset, elementSize) != 0) return ThrowRangeError("start offset must be a multiple of the element size");
  bool bufferIsFixedLength = !buffer->isResizable();
  double newLength = 0;
  if (!length.isUndefined()) newLength = TRY(ToIndex(length));
  if (buffer->isDetached()) return ThrowTypeError("buffer is detached");
  double bufferByteLength = double(buffer->byteLength());
  if (length.isUndefined() && !bufferIsFixedLength) {
    if (offset > bufferByteLength) return ThrowRangeError("start offset is outside the bounds of the buffer");
    o->byteLength.reset();
    o->arrayLength.reset();
  } else {
    double newByteLength;
    if (length.isUndefined()) {
      if (std::fmod(bufferByteLength, elementSize) != 0) return ThrowRangeError("buffer length must be a multiple of the element size");
      newByteLength = bufferByteLength - offset;
      if (newByteLength < 0) return ThrowRangeError("start offset is outside the bounds of the buffer");
    } else {
      newByteLength = newLength * elementSize;
      if (offset + newByteLength > bufferByteLength) return ThrowRangeError("invalid typed array length");
    }
    o->byteLength = newByteLength;
    o->arrayLength = newByteLength / elementSize;
  }
  o->buffer = buffer;
  o->byteOffset = offset;
  return {};
}

// 23.2.5.1 TypedArray ( ...args ): the callee's first slot holds its ElementType.
Result<Value> TypedArray_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor requires 'new'");
  auto type = ElementType(uint8_t(call.callee->slots[0].asNumber()));
  if (call.args.size() == 0) return Value(TRY(AllocateTypedArray(type, call.newTarget, 0.0)));
  Value first = call.args[0];
  if (!first.isObject()) {
    double elementLength = TRY(ToIndex(first));
    return Value(TRY(AllocateTypedArray(type, call.newTarget, elementLength)));
  }
  TypedArrayObject* o = TRY(AllocateTypedArray(type, call.newTarget, std::nullopt));
  Object* firstObject = first.asObject();
  if (auto* src = firstObject->as<TypedArrayObject>()) {
    TRY(InitializeTypedArrayFromTypedArray(o, src));
  } else if (auto* buffer = firstObject->as<ArrayBufferObject>()) {
    TRY(InitializeTypedArrayFromArrayBuffer(o, buffer, call.args[1], call.args[2]));
  } else {
    Value usingIterator = TRY(GetMethod(first, PropertyKey(symbols().iterator)));
    ValueVector values;
    if (!usingIterator.isUndefined()) {
      IteratorRecord record = TRY(GetIteratorFromMethod(first, usingIterator));
      values = TRY(IteratorToList(record));
      TRY(AllocateTypedArrayBuffer(o, double(values.size())));
      for (size_t k = 0; k < values.size(); k++) TRY(Set(o, PropertyKey::index(uint32_t(k)), values[k], true));
    } else {
      double len = TRY(LengthOfArrayLike(firstObject));
      TRY(AllocateTypedArrayBuffer(o, len));
      for (double k = 0; k < len; k++) {
        Value kValue = TRY(Get(firstObject, indexKey(k)));
        TRY(Set(o, indexKey(k), kValue, true));
      }
    }
  }
  return Value(o);
}

// 23.2.1.1 %TypedArray% ( )
Result<Value> AbstractTypedArray_construct(NativeCall&) { return ThrowTypeError("Abstract class TypedArray not directly constructable"); }

// 23.2.2.1 %TypedArray%.from ( source [ , mapper [ , thisArg ] ] )
Result<Value> TypedArray_from(NativeCall& call) {
  Value c = call.thisValue;
  if (!IsConstructor(c)) return ThrowTypeError("TypedArray.from: this is not a constructor");
  Value mapfn = call.args[1], thisArg = call.args[2];
  bool mapping = !mapfn.isUndefined();
  if (mapping && !IsCallable(mapfn)) return ThrowTypeError("TypedArray.from: mapper is not a function");
  Value source = call.args[0];
  Value usingIterator = TRY(GetMethod(source, PropertyKey(symbols().iterator)));
  if (!usingIterator.isUndefined()) {
    IteratorRecord record = TRY(GetIteratorFromMethod(source, usingIterator));
    ValueVector values = TRY(IteratorToList(record));
    double len = double(values.size());
    TypedArrayObject* target = TRY(TypedArrayCreateFromConstructor(c.asObject(), {num(len)}, AccessMode::Write));
    for (size_t k = 0; k < values.size(); k++) {
      Value mappedValue = mapping ? TRY(Call(mapfn, thisArg, {values[k], num(double(k))})) : values[k];
      TRY(Set(target, PropertyKey::index(uint32_t(k)), mappedValue, true));
    }
    return Value(target);
  }
  Object* arrayLike = TRY(ToObject(source));
  double len = TRY(LengthOfArrayLike(arrayLike));
  TypedArrayObject* target = TRY(TypedArrayCreateFromConstructor(c.asObject(), {num(len)}, AccessMode::Write));
  for (double k = 0; k < len; k++) {
    Value kValue = TRY(Get(arrayLike, indexKey(k)));
    Value mappedValue = mapping ? TRY(Call(mapfn, thisArg, {kValue, num(k)})) : kValue;
    TRY(Set(target, indexKey(k), mappedValue, true));
  }
  return Value(target);
}

// 23.2.2.2 %TypedArray%.of ( ...items )
Result<Value> TypedArray_of(NativeCall& call) {
  Value c = call.thisValue;
  if (!IsConstructor(c)) return ThrowTypeError("TypedArray.of: this is not a constructor");
  TypedArrayObject* newObj =
      TRY(TypedArrayCreateFromConstructor(c.asObject(), {num(double(call.args.size()))}, AccessMode::Write));
  for (size_t k = 0; k < call.args.size(); k++) TRY(Set(newObj, PropertyKey::index(uint32_t(k)), call.args[k], true));
  return Value(newObj);
}

Result<Value> TypedArray_species(NativeCall& call) { return call.thisValue; }

Result<TypedArrayObject*> requireTypedArray(Value v) {
  TypedArrayObject* ta = v.isObject() ? v.asObject()->as<TypedArrayObject>() : nullptr;
  if (!ta) return ThrowTypeError("this is not a typed array.");
  return ta;
}

Result<Value> TypedArrayPrototype_buffer(NativeCall& call) {
  TypedArrayObject* o = TRY(requireTypedArray(call.thisValue));
  return Value(o->buffer);
}

Result<Value> TypedArrayPrototype_byteLength(NativeCall& call) {
  TypedArrayObject* o = TRY(requireTypedArray(call.thisValue));
  return num(TypedArrayByteLength(o));
}

Result<Value> TypedArrayPrototype_byteOffset(NativeCall& call) {
  TypedArrayObject* o = TRY(requireTypedArray(call.thisValue));
  if (IsTypedArrayOutOfBounds(o)) return num(0);
  return num(o->byteOffset);
}

Result<Value> TypedArrayPrototype_length(NativeCall& call) {
  TypedArrayObject* o = TRY(requireTypedArray(call.thisValue));
  return num(TypedArrayLength(o));
}

// 23.2.3.38 get %TypedArray%.prototype [ @@toStringTag ]
Result<Value> TypedArrayPrototype_toStringTag(NativeCall& call) {
  Value o = call.thisValue;
  if (!o.isObject()) return Value::undefined();
  auto* ta = o.asObject()->as<TypedArrayObject>();
  if (!ta) return Value::undefined();
  return str(TypedArrayName(ta->elementType));
}

struct Validated {
  TypedArrayObject* o;
  double len;
};

Result<Validated> validate(Value thisValue, AccessMode accessMode = AccessMode::Read) {
  TypedArrayObject* o = TRY(ValidateTypedArray(thisValue, accessMode));
  return Validated{o, TypedArrayLength(o)};
}

// 23.2.3.1 %TypedArray%.prototype.at ( index )
Result<Value> TypedArrayPrototype_at(NativeCall& call) {
  auto [o, len] = TRY(validate(call.thisValue));
  double relativeIndex = TRY(ToIntegerOrInfinity(call.args[0]));
  double k = relativeIndex >= 0 ? relativeIndex : len + relativeIndex;
  if (k < 0 || k >= len) return Value::undefined();
  return Get(o, indexKey(k));
}

// 23.2.3.6 %TypedArray%.prototype.copyWithin ( target, start [ , end ] )
Result<Value> TypedArrayPrototype_copyWithin(NativeCall& call) {
  auto [o, len] = TRY(validate(call.thisValue, AccessMode::Write));
  double targetIndex = relativeIndex(TRY(ToIntegerOrInfinity(call.args[0])), len);
  double startIndex = relativeIndex(TRY(ToIntegerOrInfinity(call.args[1])), len);
  double relativeEnd = call.args[2].isUndefined() ? len : TRY(ToIntegerOrInfinity(call.args[2]));
  double endIndex = relativeIndex(relativeEnd, len);
  double count = std::min(endIndex - startIndex, len - targetIndex);
  if (count > 0) {
    if (IsTypedArrayOutOfBounds(o)) return ThrowTypeError("typed array is detached or out of bounds");
    len = TypedArrayLength(o);
    // Side effects may have shrunk O: copy the longest still-applicable prefix.
    count = std::min({count, len - startIndex, len - targetIndex});
    if (count > 0) {
      size_t elementSize = ElementSize(o->elementType);
      uint8_t* data = o->buffer->data() + size_t(o->byteOffset);
      std::memmove(data + size_t(targetIndex) * elementSize, data + size_t(startIndex) * elementSize, size_t(count) * elementSize);
    }
  }
  return Value(o);
}

Result<Value> iteratorOf(NativeCall& call, int kind) {
  TypedArrayObject* o = TRY(ValidateTypedArray(call.thisValue));
  return Value(CreateArrayIteratorForKind(o, kind));
}
Result<Value> TypedArrayPrototype_keys(NativeCall& call) { return iteratorOf(call, 0); }
Result<Value> TypedArrayPrototype_values(NativeCall& call) { return iteratorOf(call, 1); }
Result<Value> TypedArrayPrototype_entries(NativeCall& call) { return iteratorOf(call, 2); }

enum class Each { Every, Some, ForEach, Find, FindIndex, FindLast, FindLastIndex };

// every, some, forEach, find, findIndex, findLast, findLastIndex: Get each element (no HasProperty).
template <Each K>
Result<Value> each(NativeCall& call) {
  auto [o, len] = TRY(validate(call.thisValue));
  Value fn = call.args[0];
  if (!IsCallable(fn)) return ThrowTypeError("callback is not a function");
  bool backwards = K == Each::FindLast || K == Each::FindLastIndex;
  for (double i = 0; i < len; i++) {
    double k = backwards ? len - 1 - i : i;
    Value kValue = TRY(Get(o, indexKey(k)));
    Value result = TRY(Call(fn, call.args[1], {kValue, num(k), Value(o)}));
    bool truthy = ToBoolean(result);
    if (K == Each::Every && !truthy) return boolean(false);
    if (K == Each::Some && truthy) return boolean(true);
    if ((K == Each::Find || K == Each::FindLast) && truthy) return kValue;
    if ((K == Each::FindIndex || K == Each::FindLastIndex) && truthy) return num(k);
  }
  if (K == Each::Every) return boolean(true);
  if (K == Each::Some) return boolean(false);
  if (K == Each::FindIndex || K == Each::FindLastIndex) return num(-1);
  return Value::undefined();
}

// 23.2.3.9 %TypedArray%.prototype.fill ( value [ , start [ , end ] ] )
Result<Value> TypedArrayPrototype_fill(NativeCall& call) {
  auto [o, len] = TRY(validate(call.thisValue, AccessMode::Write));
  Value value = TRY(ToElementValue(o->elementType, call.args[0]));
  double k = relativeIndex(TRY(ToIntegerOrInfinity(call.args[1])), len);
  double relativeEnd = call.args[2].isUndefined() ? len : TRY(ToIntegerOrInfinity(call.args[2]));
  double endIndex = relativeIndex(relativeEnd, len);
  if (IsTypedArrayOutOfBounds(o)) return ThrowTypeError("typed array is detached or out of bounds");
  endIndex = std::min(endIndex, TypedArrayLength(o));
  for (; k < endIndex; k++) TRY(TypedArraySetElement(o, k, value));
  return Value(o);
}

// 23.2.3.10 %TypedArray%.prototype.filter ( callback [ , thisArg ] )
Result<Value> TypedArrayPrototype_filter(NativeCall& call) {
  auto [o, len] = TRY(validate(call.thisValue));
  Value fn = call.args[0];
  if (!IsCallable(fn)) return ThrowTypeError("callback is not a function");
  ValueVector kept;
  for (double k = 0; k < len; k++) {
    Value kValue = TRY(Get(o, indexKey(k)));
    Value selected = TRY(Call(fn, call.args[1], {kValue, num(k), Value(o)}));
    if (ToBoolean(selected)) kept.push_back(kValue);
  }
  TypedArrayObject* a = TRY(TypedArraySpeciesCreate(o, {num(double(kept.size()))}, AccessMode::Write));
  for (size_t n = 0; n < kept.size(); n++) TRY(Set(a, PropertyKey::index(uint32_t(n)), kept[n], true));
  return Value(a);
}

// 23.2.3.21 %TypedArray%.prototype.map ( callback [ , thisArg ] )
Result<Value> TypedArrayPrototype_map(NativeCall& call) {
  auto [o, len] = TRY(validate(call.thisValue));
  Value fn = call.args[0];
  if (!IsCallable(fn)) return ThrowTypeError("callback is not a function");
  TypedArrayObject* a = TRY(TypedArraySpeciesCreate(o, {num(len)}, AccessMode::Write));
  for (double k = 0; k < len; k++) {
    Value kValue = TRY(Get(o, indexKey(k)));
    Value mappedValue = TRY(Call(fn, call.args[1], {kValue, num(k), Value(o)}));
    TRY(Set(a, indexKey(k), mappedValue, true));
  }
  return Value(a);
}

// 23.2.3.22 / 23.2.3.23 reduce and reduceRight
template <bool Right>
Result<Value> reduce(NativeCall& call) {
  auto [o, len] = TRY(validate(call.thisValue));
  Value fn = call.args[0];
  if (!IsCallable(fn)) return ThrowTypeError("callback is not a function");
  if (len == 0 && call.args.size() < 2) return ThrowTypeError("Reduce of empty array with no initial value");
  double i = 0;
  Value accumulator;
  if (call.args.size() >= 2) {
    accumulator = call.args[1];
  } else {
    accumulator = TRY(Get(o, indexKey(Right ? len - 1 : 0)));
    i = 1;
  }
  for (; i < len; i++) {
    double k = Right ? len - 1 - i : i;
    Value kValue = TRY(Get(o, indexKey(k)));
    accumulator = TRY(Call(fn, Value::undefined(), {accumulator, kValue, num(k), Value(o)}));
  }
  return accumulator;
}

// 23.2.3.16 includes, 23.2.3.17 indexOf, 23.2.3.20 lastIndexOf
enum class Search { Includes, IndexOf, LastIndexOf };

template <Search S>
Result<Value> search(NativeCall& call) {
  auto [o, len] = TRY(validate(call.thisValue));
  Value notFound = S == Search::Includes ? boolean(false) : num(-1);
  if (len == 0) return notFound;
  if (S == Search::LastIndexOf) {
    double n = call.args.size() > 1 ? TRY(ToIntegerOrInfinity(call.args[1])) : len - 1;
    if (n == -INFINITY) return notFound;
    for (double k = n >= 0 ? std::min(n, len - 1) : len + n; k >= 0; k--) {
      if (!IsValidIntegerIndex(o, k)) continue;
      Value elementK = TRY(Get(o, indexKey(k)));
      if (IsStrictlyEqual(call.args[0], elementK)) return num(k);
    }
    return notFound;
  }
  double n = TRY(ToIntegerOrInfinity(call.args[1]));
  if (n == INFINITY) return notFound;
  if (n == -INFINITY) n = 0;
  for (double k = n >= 0 ? n : std::max(len + n, 0.0); k < len; k++) {
    if (S == Search::IndexOf && !IsValidIntegerIndex(o, k)) continue;
    Value elementK = TRY(Get(o, indexKey(k)));
    if (S == Search::Includes && SameValueZero(call.args[0], elementK)) return boolean(true);
    if (S == Search::IndexOf && IsStrictlyEqual(call.args[0], elementK)) return num(k);
  }
  return notFound;
}

// 23.2.3.18 %TypedArray%.prototype.join ( separator )
Result<Value> TypedArrayPrototype_join(NativeCall& call) {
  auto [o, len] = TRY(validate(call.thisValue));
  String* sep = call.args[0].isUndefined() ? atom(u",") : TRY(ToString(call.args[0]));
  std::u16string r;
  for (double k = 0; k < len; k++) {
    if (k > 0) r.append(sep->view());
    Value element = TRY(Get(o, indexKey(k)));
    if (!element.isUndefined()) r.append(TRY(ToString(element))->view());
  }
  return Value(newString(std::move(r)));
}

// 23.2.3.25 %TypedArray%.prototype.reverse ( )
Result<Value> TypedArrayPrototype_reverse(NativeCall& call) {
  auto [o, len] = TRY(validate(call.thisValue, AccessMode::Write));
  double middle = std::floor(len / 2);
  for (double lower = 0; lower != middle; lower++) {
    double upper = len - lower - 1;
    Value lowerValue = TypedArrayGetElement(o, lower), upperValue = TypedArrayGetElement(o, upper);
    TRY(TypedArraySetElement(o, lower, upperValue));
    TRY(TypedArraySetElement(o, upper, lowerValue));
  }
  return Value(o);
}

// 23.2.3.26.1 SetTypedArrayFromTypedArray ( target, targetOffset, source )
Result<void> SetTypedArrayFromTypedArray(TypedArrayObject* target, double targetOffset, TypedArrayObject* source) {
  if (IsTypedArrayOutOfBounds(target)) return ThrowTypeError("target typed array is detached or out of bounds");
  double targetLength = TypedArrayLength(target);
  if (IsTypedArrayOutOfBounds(source)) return ThrowTypeError("source typed array is detached or out of bounds");
  double srcLength = TypedArrayLength(source);
  if (targetOffset == INFINITY) return ThrowRangeError("offset is out of bounds");
  if (srcLength + targetOffset > targetLength) return ThrowRangeError("offset is out of bounds");
  if (!sameContentType(target->elementType, source->elementType)) return ThrowTypeError("Content types differ");
  size_t srcElementSize = ElementSize(source->elementType), targetElementSize = ElementSize(target->elementType);
  // Copy the source bytes first: the two may share a buffer.
  std::vector<uint8_t> srcBytes(source->buffer->data() + size_t(source->byteOffset),
                                source->buffer->data() + size_t(source->byteOffset) + size_t(srcLength) * srcElementSize);
  size_t targetByteIndex = size_t(targetOffset) * targetElementSize + size_t(target->byteOffset);
  if (source->elementType == target->elementType) {
    std::memcpy(target->buffer->data() + targetByteIndex, srcBytes.data(), srcBytes.size());
    return {};
  }
  ArrayBufferObject scratch(nullptr, false, srcBytes.size(), std::nullopt);
  std::memcpy(scratch.data(), srcBytes.data(), srcBytes.size());
  for (size_t i = 0; i < size_t(srcLength); i++) {
    Value value = GetValueFromBuffer(&scratch, i * srcElementSize, source->elementType);
    SetValueInBuffer(target->buffer, targetByteIndex + i * targetElementSize, target->elementType, value);
  }
  return {};
}

// 23.2.3.26 %TypedArray%.prototype.set ( source [ , offset ] )
Result<Value> TypedArrayPrototype_set(NativeCall& call) {
  TypedArrayObject* target = TRY(requireTypedArray(call.thisValue));
  if (IsImmutableBuffer(target->buffer)) return ThrowTypeError("typed array is backed by an immutable ArrayBuffer");
  double targetOffset = TRY(ToIntegerOrInfinity(call.args[1]));
  if (targetOffset < 0) return ThrowRangeError("offset must be >= 0");
  Value source = call.args[0];
  if (auto* src = source.isObject() ? source.asObject()->as<TypedArrayObject>() : nullptr) {
    TRY(SetTypedArrayFromTypedArray(target, targetOffset, src));
    return Value::undefined();
  }
  // 23.2.3.26.2 SetTypedArrayFromArrayLike
  if (IsTypedArrayOutOfBounds(target)) return ThrowTypeError("target typed array is detached or out of bounds");
  double targetLength = TypedArrayLength(target);
  Object* src = TRY(ToObject(source));
  double srcLength = TRY(LengthOfArrayLike(src));
  if (targetOffset == INFINITY) return ThrowRangeError("offset is out of bounds");
  if (srcLength + targetOffset > targetLength) return ThrowRangeError("offset is out of bounds");
  for (double k = 0; k < srcLength; k++) {
    Value value = TRY(Get(src, indexKey(k)));
    TRY(TypedArraySetElement(target, targetOffset + k, value));
  }
  return Value::undefined();
}

// 23.2.3.27 %TypedArray%.prototype.slice ( start, end )
Result<Value> TypedArrayPrototype_slice(NativeCall& call) {
  auto [o, srcArrayLength] = TRY(validate(call.thisValue));
  double startIndex = relativeIndex(TRY(ToIntegerOrInfinity(call.args[0])), srcArrayLength);
  double relativeEnd = call.args[1].isUndefined() ? srcArrayLength : TRY(ToIntegerOrInfinity(call.args[1]));
  double endIndex = relativeIndex(relativeEnd, srcArrayLength);
  double countBytes = std::max(endIndex - startIndex, 0.0);
  TypedArrayObject* a = TRY(TypedArraySpeciesCreate(o, {num(countBytes)}, AccessMode::Write));
  if (countBytes > 0) {
    if (IsTypedArrayOutOfBounds(o)) return ThrowTypeError("typed array is detached or out of bounds");
    endIndex = std::min(endIndex, TypedArrayLength(o));
    countBytes = std::max(endIndex - startIndex, 0.0);
    if (o->elementType == a->elementType) {
      size_t elementSize = ElementSize(o->elementType);
      size_t srcByteIndex = size_t(startIndex) * elementSize + size_t(o->byteOffset);
      size_t targetByteIndex = size_t(a->byteOffset);
      size_t count = size_t(countBytes) * elementSize;
      size_t limit = std::min(count, a->buffer->byteLength() - std::min(targetByteIndex, a->buffer->byteLength()));
      // Byte by byte, front to back, as the spec copies (it matters when A shares O's buffer).
      uint8_t* to = a->buffer->data() + targetByteIndex;
      const uint8_t* from = o->buffer->data() + srcByteIndex;
      for (size_t i = 0; i < limit; i++) to[i] = from[i];
    } else {
      double n = 0;
      for (double k = startIndex; k < endIndex; k++, n++) {
        Value kValue = TypedArrayGetElement(o, k);
        TRY(TypedArraySetElement(a, n, kValue));
      }
    }
  }
  return Value(a);
}

// 23.2.4.7 CompareTypedArrayElements ( x, y, comparator )
Result<double> CompareTypedArrayElements(Value x, Value y, Value comparator) {
  if (!comparator.isUndefined()) {
    Value r = TRY(Call(comparator, Value::undefined(), {x, y}));
    double v = TRY(ToNumber(r));
    return std::isnan(v) ? 0.0 : v;
  }
  if (x.isBigInt()) {
    int c = BigInt::compare(x.asBigInt(), y.asBigInt());
    return double(c);
  }
  double a = x.asNumber(), b = y.asNumber();
  if (std::isnan(a) && std::isnan(b)) return 0.0;
  if (std::isnan(a)) return 1.0;
  if (std::isnan(b)) return -1.0;
  if (a < b) return -1.0;
  if (a > b) return 1.0;
  if (a == 0 && b == 0) {
    if (std::signbit(a) && !std::signbit(b)) return -1.0;
    if (!std::signbit(a) && std::signbit(b)) return 1.0;
  }
  return 0.0;
}

// The elements of o, sorted stably with CompareTypedArrayElements (a merge sort whose comparator may throw).
Result<ValueVector> sortedElements(TypedArrayObject* o, double len, Value comparator) {
  ValueVector items;
  for (double k = 0; k < len; k++) items.push_back(TypedArrayGetElement(o, k));
  size_t n = items.size();
  ValueVector buffer;
  buffer.resize(n);
  for (size_t width = 1; width < n; width *= 2) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      size_t mid = std::min(lo + width, n), hi = std::min(lo + 2 * width, n);
      size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        double c = TRY(CompareTypedArrayElements(items[j], items[i], comparator));
        buffer[k++] = c < 0 ? items[j++] : items[i++];
      }
      while (i < mid) buffer[k++] = items[i++];
      while (j < hi) buffer[k++] = items[j++];
    }
    for (size_t i = 0; i < n; i++) items[i] = buffer[i];
  }
  return items;
}

// 23.2.3.29 %TypedArray%.prototype.sort ( comparator )
Result<Value> TypedArrayPrototype_sort(NativeCall& call) {
  Value comparator = call.args[0];
  if (!comparator.isUndefined() && !IsCallable(comparator)) return ThrowTypeError("The comparison function must be either a function or undefined");
  auto [o, len] = TRY(validate(call.thisValue, AccessMode::Write));
  ValueVector sorted = TRY(sortedElements(o, len, comparator));
  for (size_t j = 0; j < sorted.size(); j++) TRY(TypedArraySetElement(o, double(j), sorted[j]));
  return Value(o);
}

// 23.2.3.30 %TypedArray%.prototype.subarray ( start, end )
Result<Value> TypedArrayPrototype_subarray(NativeCall& call) {
  TypedArrayObject* o = TRY(requireTypedArray(call.thisValue));
  double srcLength = IsTypedArrayOutOfBounds(o) ? 0 : TypedArrayLength(o);
  double startIndex = relativeIndex(TRY(ToIntegerOrInfinity(call.args[0])), srcLength);
  double elementSize = double(ElementSize(o->elementType));
  double beginByteOffset = o->byteOffset + startIndex * elementSize;
  if (!o->arrayLength && call.args[1].isUndefined())
    return Value(TRY(TypedArraySpeciesCreate(o, {Value(o->buffer), num(beginByteOffset)})));
  double relativeEnd = call.args[1].isUndefined() ? srcLength : TRY(ToIntegerOrInfinity(call.args[1]));
  double endIndex = relativeIndex(relativeEnd, srcLength);
  double newLength = std::max(endIndex - startIndex, 0.0);
  return Value(TRY(TypedArraySpeciesCreate(o, {Value(o->buffer), num(beginByteOffset), num(newLength)})));
}

// 23.2.3.31 %TypedArray%.prototype.toLocaleString ( [ reserved1 [ , reserved2 ] ] ), as ECMA-402 22.2.1 specifies
// it: locales and options are passed on to each element's toLocaleString.
Result<Value> TypedArrayPrototype_toLocaleString(NativeCall& call) {
  auto [o, len] = TRY(validate(call.thisValue));
  std::u16string r;
  for (double k = 0; k < len; k++) {
    if (k > 0) r.push_back(u',');
    Value nextElement = TRY(Get(o, indexKey(k)));
    if (!nextElement.isNullish()) {
      Value s = TRY(Invoke(nextElement, PropertyKey(u"toLocaleString"), {call.args[0], call.args[1]}));
      r.append(TRY(ToString(s))->view());
    }
  }
  return Value(newString(std::move(r)));
}

// 23.2.3.32 %TypedArray%.prototype.toReversed ( )
Result<Value> TypedArrayPrototype_toReversed(NativeCall& call) {
  auto [o, len] = TRY(validate(call.thisValue));
  TypedArrayObject* a = TRY(TypedArrayCreateSameType(o, {num(len)}));
  for (double k = 0; k < len; k++) TRY(TypedArraySetElement(a, k, TypedArrayGetElement(o, len - k - 1)));
  return Value(a);
}

// 23.2.3.33 %TypedArray%.prototype.toSorted ( comparator )
Result<Value> TypedArrayPrototype_toSorted(NativeCall& call) {
  Value comparator = call.args[0];
  if (!comparator.isUndefined() && !IsCallable(comparator)) return ThrowTypeError("The comparison function must be either a function or undefined");
  auto [o, len] = TRY(validate(call.thisValue));
  TypedArrayObject* a = TRY(TypedArrayCreateSameType(o, {num(len)}));
  ValueVector sorted = TRY(sortedElements(o, len, comparator));
  for (size_t j = 0; j < sorted.size(); j++) TRY(TypedArraySetElement(a, double(j), sorted[j]));
  return Value(a);
}

// 23.2.3.36 %TypedArray%.prototype.with ( index, value )
Result<Value> TypedArrayPrototype_with(NativeCall& call) {
  auto [o, len] = TRY(validate(call.thisValue));
  double relativeIndex = TRY(ToIntegerOrInfinity(call.args[0]));
  double actualIndex = relativeIndex >= 0 ? relativeIndex : len + relativeIndex;
  Value numericValue = TRY(ToElementValue(o->elementType, call.args[1]));
  if (!IsValidIntegerIndex(o, actualIndex)) return ThrowRangeError("Invalid typed array index");
  TypedArrayObject* a = TRY(TypedArrayCreateSameType(o, {num(len)}));
  for (double k = 0; k < len; k++) {
    Value fromValue = k == actualIndex ? numericValue : TypedArrayGetElement(o, k);
    TRY(TypedArraySetElement(a, k, fromValue));
  }
  return Value(a);
}

void installTypedArrays(Realm& realm) {
  arrayIteratorLength = typedArrayAwareLength;
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* abstract = defineConstructor(realm, Intrinsic::TypedArray, AbstractTypedArray_construct, 0,
                                                PropertyKey(u"TypedArray"), proto, Intrinsic::TypedArrayPrototype, false);
  defineFunction(realm, abstract, PropertyKey(u"from"), 1, TypedArray_from);
  defineFunction(realm, abstract, PropertyKey(u"of"), 0, TypedArray_of);
  defineAccessor(realm, abstract, PropertyKey(symbols().species), TypedArray_species, nullptr);

  defineAccessor(realm, proto, PropertyKey(u"buffer"), TypedArrayPrototype_buffer, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"byteLength"), TypedArrayPrototype_byteLength, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"byteOffset"), TypedArrayPrototype_byteOffset, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"length"), TypedArrayPrototype_length, nullptr);
  struct Fn {
    const char16_t* name;
    int length;
    NativeFn fn;
  };
  const Fn fns[] = {
      {u"at", 1, TypedArrayPrototype_at},
      {u"copyWithin", 2, TypedArrayPrototype_copyWithin},
      {u"entries", 0, TypedArrayPrototype_entries},
      {u"every", 1, each<Each::Every>},
      {u"fill", 1, TypedArrayPrototype_fill},
      {u"filter", 1, TypedArrayPrototype_filter},
      {u"find", 1, each<Each::Find>},
      {u"findIndex", 1, each<Each::FindIndex>},
      {u"findLast", 1, each<Each::FindLast>},
      {u"findLastIndex", 1, each<Each::FindLastIndex>},
      {u"forEach", 1, each<Each::ForEach>},
      {u"includes", 1, search<Search::Includes>},
      {u"indexOf", 1, search<Search::IndexOf>},
      {u"join", 1, TypedArrayPrototype_join},
      {u"keys", 0, TypedArrayPrototype_keys},
      {u"lastIndexOf", 1, search<Search::LastIndexOf>},
      {u"map", 1, TypedArrayPrototype_map},
      {u"reduce", 1, reduce<false>},
      {u"reduceRight", 1, reduce<true>},
      {u"reverse", 0, TypedArrayPrototype_reverse},
      {u"set", 1, TypedArrayPrototype_set},
      {u"slice", 2, TypedArrayPrototype_slice},
      {u"some", 1, each<Each::Some>},
      {u"sort", 1, TypedArrayPrototype_sort},
      {u"subarray", 2, TypedArrayPrototype_subarray},
      {u"toLocaleString", 0, TypedArrayPrototype_toLocaleString},
      {u"toReversed", 0, TypedArrayPrototype_toReversed},
      {u"toSorted", 1, TypedArrayPrototype_toSorted},
      {u"with", 2, TypedArrayPrototype_with},
  };
  for (const Fn& f : fns) defineFunction(realm, proto, PropertyKey(f.name), f.length, f.fn);
  // %TypedArray%.prototype.toString is %Array.prototype.toString% (installed earlier: kOrderLate).
  defineValue(proto, PropertyKey(u"toString"), Get(realm.intrinsic(Intrinsic::ArrayPrototype), PropertyKey(u"toString")).release());
  BuiltinFunction* values = defineFunction(realm, proto, PropertyKey(u"values"), 0, TypedArrayPrototype_values);
  defineValue(proto, PropertyKey(symbols().iterator), values);
  defineAccessor(realm, proto, PropertyKey(symbols().toStringTag), TypedArrayPrototype_toStringTag, nullptr);

  for (ElementType type : kAllTypes) {
    Object* typedProto = OrdinaryObjectCreate(proto);
    BuiltinFunction* ctor = defineConstructor(realm, TypedArrayConstructorIntrinsic(type), TypedArray_construct, 3,
                                              PropertyKey(TypedArrayName(type)), typedProto, TypedArrayPrototypeIntrinsic(type));
    ctor->slots.push_back(num(double(uint8_t(type))));
    ctor->setPrototypeDirect(abstract);
    Value bytes = num(double(ElementSize(type)));
    defineValue(ctor, PropertyKey(u"BYTES_PER_ELEMENT"), bytes, Attr::None);
    defineValue(typedProto, PropertyKey(u"BYTES_PER_ELEMENT"), bytes, Attr::None);
  }
}

}  // namespace

SNOWMAN_BUILTIN(TypedArray, kOrderLate, installTypedArrays);

}  // namespace snowman::builtins
