// Array (23.1): the constructor, Array.prototype, and %ArrayIteratorPrototype%.
#include <cmath>
#include <typeinfo>

#include "builtins/util.h"
#include "runtime/array.h"
#include "runtime/function.h"
#include "runtime/wrappers.h"

namespace snowman::builtins {

Result<Value> ObjectPrototype_toString(NativeCall& call);

namespace {

constexpr double kMaxSafeInteger = 9007199254740991.0;
constexpr double kIndexLimit = 4294967295.0;  // integer keys below this are array indices

// Whether [[HasProperty]] on integer keys is ordinary and unobservable along the whole prototype chain,
// so loops may skip absent indices without changing behaviour.
bool simpleIndexed(Object* o) {
  const std::type_info& objectPrototypeType = typeid(*vm().currentRealm()->intrinsic(Intrinsic::ObjectPrototype));
  for (Object* p = o; p; p = p->prototype()) {
    const std::type_info& t = typeid(*p);
    if (t == typeid(Object) || t == typeid(ArrayObject) || t == typeid(BooleanObject) || t == typeid(NumberObject) ||
        t == typeid(ErrorObject) || t == objectPrototypeType)
      continue;
    return false;
  }
  return true;
}

// The first index ≥ k that may be present on o or its prototypes (len when none).
double nextPresent(Object* o, double k, double len) {
  if (k >= len || k >= kIndexLimit || !simpleIndexed(o)) return k;
  double best = std::min(len, kIndexLimit);
  for (Object* p = o; p; p = p->prototype()) {
    auto n = p->properties().nextIndex(uint32_t(k));
    if (n && *n < best) best = *n;
  }
  return best;
}

// The last index ≤ k that may be present (-1 when none).
double prevPresent(Object* o, double k) {
  if (k < 0 || k >= kIndexLimit || !simpleIndexed(o)) return k;
  double best = -1;
  for (Object* p = o; p; p = p->prototype()) {
    auto n = p->properties().prevIndex(uint32_t(k));
    if (n && double(*n) > best) best = *n;
  }
  return best;
}

Result<void> setLength(Object* o, double len) { return Set(o, PropertyKey(u"length"), num(len), true); }

// 23.1.1.1 Array ( ...values )
Result<Value> Array_construct(NativeCall& call) {
  Object* newTarget = newTargetOrCallee(call);
  Object* proto = TRY(GetPrototypeFromConstructor(newTarget, Intrinsic::ArrayPrototype));
  size_t numberOfArgs = call.args.size();
  if (numberOfArgs == 0) return Value(TRY(ArrayCreate(0, proto)));
  if (numberOfArgs == 1) {
    Value len = call.args[0];
    Object* array = TRY(ArrayCreate(0, proto));
    double intLen;
    if (!len.isNumber()) {
      TRY(CreateDataPropertyOrThrow(array, PropertyKey::index(0), len));
      intLen = 1;
    } else {
      intLen = DoubleToUint32(len.asNumber());
      if (intLen != len.asNumber()) return ThrowRangeError("Invalid array length");
    }
    TRY(setLength(array, intLen));
    return Value(array);
  }
  Object* array = TRY(ArrayCreate(double(numberOfArgs), proto));
  for (size_t k = 0; k < numberOfArgs; k++) TRY(CreateDataPropertyOrThrow(array, PropertyKey::index(uint32_t(k)), call.args[k]));
  return Value(array);
}

// 23.1.2.1 Array.from ( items [ , mapper [ , thisArg ] ] )
Result<Value> Array_from(NativeCall& call) {
  Value c = call.thisValue;
  Value items = call.args[0], mapfn = call.args[1], thisArg = call.args[2];
  bool mapping = !mapfn.isUndefined();
  if (mapping && !IsCallable(mapfn)) return ThrowTypeError("Array.from: mapper is not a function");
  Value usingIterator = TRY(GetMethod(items, PropertyKey(symbols().iterator)));
  if (!usingIterator.isUndefined()) {
    Object* a = IsConstructor(c) ? TRY(Construct(c.asObject())) : TRY(ArrayCreate(0));
    IteratorRecord record = TRY(GetIteratorFromMethod(items, usingIterator));
    double k = 0;
    while (true) {
      if (k >= kMaxSafeInteger) return closeIteratorWithThrow(record, ThrowTypeError("Array.from: too many items").value);
      std::optional<Value> next = TRY(IteratorStepValue(record));
      if (!next) {
        TRY(setLength(a, k));
        return Value(a);
      }
      Value mappedValue = *next;
      if (mapping) mappedValue = TRY_CLOSE(record, Call(mapfn, thisArg, {*next, num(k)}));
      TRY_CLOSE(record, CreateDataPropertyOrThrow(a, indexKey(k), mappedValue));
      k++;
    }
  }
  Object* arrayLike = TRY(ToObject(items));
  double len = TRY(LengthOfArrayLike(arrayLike));
  Object* a = IsConstructor(c) ? TRY(Construct(c.asObject(), {num(len)})) : TRY(ArrayCreate(len));
  for (double k = 0; k < len; k++) {
    Value kValue = TRY(Get(arrayLike, indexKey(k)));
    Value mappedValue = mapping ? TRY(Call(mapfn, thisArg, {kValue, num(k)})) : kValue;
    TRY(CreateDataPropertyOrThrow(a, indexKey(k), mappedValue));
  }
  TRY(setLength(a, len));
  return Value(a);
}

// 23.1.2.2 Array.isArray ( arg )
Result<Value> Array_isArray(NativeCall& call) { return boolean(TRY(IsArray(call.args[0]))); }

// 23.1.2.3 Array.of ( ...items )
Result<Value> Array_of(NativeCall& call) {
  double len = double(call.args.size());
  Value c = call.thisValue;
  Object* a = IsConstructor(c) ? TRY(Construct(c.asObject(), {num(len)})) : TRY(ArrayCreate(len));
  for (size_t k = 0; k < call.args.size(); k++) TRY(CreateDataPropertyOrThrow(a, PropertyKey::index(uint32_t(k)), call.args[k]));
  TRY(setLength(a, len));
  return Value(a);
}

// 23.1.2.5 get Array [ @@species ]
Result<Value> Array_species(NativeCall& call) { return call.thisValue; }

struct ArrayThis {
  Object* o;
  double len;
};

Result<ArrayThis> thisArrayLike(Value thisValue) {
  Object* o = TRY(ToObject(thisValue));
  double len = TRY(LengthOfArrayLike(o));
  return ArrayThis{o, len};
}

// 23.1.3.1 Array.prototype.at ( index )
Result<Value> ArrayPrototype_at(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  double relativeIndex = TRY(ToIntegerOrInfinity(call.args[0]));
  double k = relativeIndex >= 0 ? relativeIndex : len + relativeIndex;
  if (k < 0 || k >= len) return Value::undefined();
  return Get(o, indexKey(k));
}

// 23.1.3.2.1 IsConcatSpreadable ( O )
Result<bool> IsConcatSpreadable(Value o) {
  if (!o.isObject()) return false;
  Value spreadable = TRY(Get(o.asObject(), PropertyKey(symbols().isConcatSpreadable)));
  if (!spreadable.isUndefined()) return ToBoolean(spreadable);
  return IsArray(o);
}

// 23.1.3.2 Array.prototype.concat ( ...items )
Result<Value> ArrayPrototype_concat(NativeCall& call) {
  Object* o = TRY(ToObject(call.thisValue));
  Object* a = TRY(ArraySpeciesCreate(o, 0));
  double n = 0;
  for (size_t i = 0; i <= call.args.size(); i++) {
    Value e = i == 0 ? Value(o) : call.args[i - 1];
    bool spreadable = TRY(IsConcatSpreadable(e));
    if (spreadable) {
      Object* eo = e.asObject();
      double len = TRY(LengthOfArrayLike(eo));
      if (n + len > kMaxSafeInteger) return ThrowTypeError("Array.prototype.concat: result is too long");
      for (double k = 0; k < len; k++) {
        double next = nextPresent(eo, k, len);
        n += next - k;
        k = next;
        if (k >= len) break;
        PropertyKey p = indexKey(k);
        bool exists = TRY(HasProperty(eo, p));
        if (exists) {
          Value subElement = TRY(Get(eo, p));
          TRY(CreateDataPropertyOrThrow(a, indexKey(n), subElement));
        }
        n++;
      }
    } else {
      if (n >= kMaxSafeInteger) return ThrowTypeError("Array.prototype.concat: result is too long");
      TRY(CreateDataPropertyOrThrow(a, indexKey(n), e));
      n++;
    }
  }
  TRY(setLength(a, n));
  return Value(a);
}

// 23.1.3.4 Array.prototype.copyWithin ( target, start [ , end ] )
Result<Value> ArrayPrototype_copyWithin(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  double to = relativeIndex(TRY(ToIntegerOrInfinity(call.args[0])), len);
  double from = relativeIndex(TRY(ToIntegerOrInfinity(call.args[1])), len);
  double relativeEnd = call.args[2].isUndefined() ? len : TRY(ToIntegerOrInfinity(call.args[2]));
  double final = relativeIndex(relativeEnd, len);
  double count = std::min(final - from, len - to);
  double direction = 1;
  if (from < to && to < from + count) {
    direction = -1;
    from = from + count - 1;
    to = to + count - 1;
  }
  while (count > 0) {
    PropertyKey fromKey = indexKey(from), toKey = indexKey(to);
    bool fromPresent = TRY(HasProperty(o, fromKey));
    if (fromPresent) {
      Value fromValue = TRY(Get(o, fromKey));
      TRY(Set(o, toKey, fromValue, true));
    } else {
      TRY(DeletePropertyOrThrow(o, toKey));
    }
    from += direction;
    to += direction;
    count--;
  }
  return Value(o);
}

// %ArrayIteratorPrototype% objects (23.1.5).
enum class IterKind { Key, Value, KeyValue };

class ArrayIterator : public Object {
 public:
  ArrayIterator(Object* prototype, Object* array, IterKind kind) : Object(prototype), array(array), kind(kind) {}
  Object* array;  // nullptr once exhausted
  IterKind kind;
  double index = 0;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(reinterpret_cast<Cell*>(array));
  }
};

// 23.1.5.1 CreateArrayIterator ( array, kind )
Object* CreateArrayIterator(Object* array, IterKind kind) {
  return make<ArrayIterator>(intrinsic(Intrinsic::ArrayIteratorPrototype), array, kind);
}

Result<Value> ArrayPrototype_entries(NativeCall& call) {
  Object* o = TRY(ToObject(call.thisValue));
  return Value(CreateArrayIterator(o, IterKind::KeyValue));
}
Result<Value> ArrayPrototype_keys(NativeCall& call) {
  Object* o = TRY(ToObject(call.thisValue));
  return Value(CreateArrayIterator(o, IterKind::Key));
}
Result<Value> ArrayPrototype_values(NativeCall& call) {
  Object* o = TRY(ToObject(call.thisValue));
  return Value(CreateArrayIterator(o, IterKind::Value));
}

}  // namespace

// The length an array iterator reads: TypedArrays override this hook (they check for out-of-bounds).
Result<double> (*arrayIteratorLength)(Object* array) = nullptr;

namespace {

// 23.1.5.2.1 %ArrayIteratorPrototype%.next ( )
Result<Value> ArrayIteratorPrototype_next(NativeCall& call) {
  ArrayIterator* it = call.thisValue.isObject() ? call.thisValue.asObject()->as<ArrayIterator>() : nullptr;
  if (!it) return ThrowTypeError("next method called on incompatible receiver");
  Object* array = it->array;
  if (!array) return Value(CreateIteratorResultObject(Value::undefined(), true));
  Result<double> lenResult = arrayIteratorLength ? arrayIteratorLength(array) : LengthOfArrayLike(array);
  if (lenResult.isThrow()) {
    it->array = nullptr;
    return Throw{lenResult.thrown()};
  }
  double len = lenResult.value();
  double index = it->index;
  if (index >= len) {
    it->array = nullptr;
    return Value(CreateIteratorResultObject(Value::undefined(), true));
  }
  it->index = index + 1;
  if (it->kind == IterKind::Key) return Value(CreateIteratorResultObject(num(index), false));
  Result<Value> elementValue = Get(array, indexKey(index));
  if (elementValue.isThrow()) {
    it->array = nullptr;
    return Throw{elementValue.thrown()};
  }
  if (it->kind == IterKind::Value) return Value(CreateIteratorResultObject(elementValue.value(), false));
  return Value(CreateIteratorResultObject(CreateArrayFromList({num(index), elementValue.value()}), false));
}

Result<Value> requireCallable(Value fn, const char* what) {
  if (!IsCallable(fn)) return ThrowTypeError(std::string(what) + ": callback is not a function");
  return fn;
}

// every / some / forEach (23.1.3.6, 23.1.3.29, 23.1.3.15)
enum class Iteration { Every, Some, ForEach };

Result<Value> iterate(NativeCall& call, Iteration kind, const char* name) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  Value callback = TRY(requireCallable(call.args[0], name));
  Value thisArg = call.args[1];
  for (double k = 0; k < len; k++) {
    k = nextPresent(o, k, len);
    if (k >= len) break;
    PropertyKey pk = indexKey(k);
    bool kPresent = TRY(HasProperty(o, pk));
    if (!kPresent) continue;
    Value kValue = TRY(Get(o, pk));
    Value result = TRY(Call(callback, thisArg, {kValue, num(k), Value(o)}));
    if (kind == Iteration::Every && !ToBoolean(result)) return boolean(false);
    if (kind == Iteration::Some && ToBoolean(result)) return boolean(true);
  }
  if (kind == Iteration::Every) return boolean(true);
  if (kind == Iteration::Some) return boolean(false);
  return Value::undefined();
}

Result<Value> ArrayPrototype_every(NativeCall& call) { return iterate(call, Iteration::Every, "Array.prototype.every"); }
Result<Value> ArrayPrototype_some(NativeCall& call) { return iterate(call, Iteration::Some, "Array.prototype.some"); }
Result<Value> ArrayPrototype_forEach(NativeCall& call) { return iterate(call, Iteration::ForEach, "Array.prototype.forEach"); }

// 23.1.3.7 Array.prototype.fill ( value [ , start [ , end ] ] )
Result<Value> ArrayPrototype_fill(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  double k = relativeIndex(TRY(ToIntegerOrInfinity(call.args[1])), len);
  double relativeEnd = call.args[2].isUndefined() ? len : TRY(ToIntegerOrInfinity(call.args[2]));
  double final = relativeIndex(relativeEnd, len);
  for (; k < final; k++) TRY(Set(o, indexKey(k), call.args[0], true));
  return Value(o);
}

// 23.1.3.8 Array.prototype.filter ( callback [ , thisArg ] )
Result<Value> ArrayPrototype_filter(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  Value callback = TRY(requireCallable(call.args[0], "Array.prototype.filter"));
  Object* a = TRY(ArraySpeciesCreate(o, 0));
  double to = 0;
  for (double k = 0; k < len; k++) {
    k = nextPresent(o, k, len);
    if (k >= len) break;
    PropertyKey pk = indexKey(k);
    bool kPresent = TRY(HasProperty(o, pk));
    if (!kPresent) continue;
    Value kValue = TRY(Get(o, pk));
    Value selected = TRY(Call(callback, call.args[1], {kValue, num(k), Value(o)}));
    if (ToBoolean(selected)) {
      TRY(CreateDataPropertyOrThrow(a, indexKey(to), kValue));
      to++;
    }
  }
  return Value(a);
}

// 23.1.3.12.1 FindViaPredicate: find, findIndex, findLast, findLastIndex
Result<Value> findVia(NativeCall& call, bool ascending, bool wantIndex) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  Value predicate = TRY(requireCallable(call.args[0], "Array.prototype.find"));
  for (double i = 0; i < len; i++) {
    double k = ascending ? i : len - 1 - i;
    Value kValue = TRY(Get(o, indexKey(k)));
    Value testResult = TRY(Call(predicate, call.args[1], {kValue, num(k), Value(o)}));
    if (ToBoolean(testResult)) return wantIndex ? num(k) : kValue;
  }
  return wantIndex ? num(-1) : Value::undefined();
}

Result<Value> ArrayPrototype_find(NativeCall& call) { return findVia(call, true, false); }
Result<Value> ArrayPrototype_findIndex(NativeCall& call) { return findVia(call, true, true); }
Result<Value> ArrayPrototype_findLast(NativeCall& call) { return findVia(call, false, false); }
Result<Value> ArrayPrototype_findLastIndex(NativeCall& call) { return findVia(call, false, true); }

// 23.1.3.13.1 FlattenIntoArray
Result<double> FlattenIntoArray(Object* target, Object* source, double sourceLen, double start, double depth,
                                Value mapperFunction = Value::undefined(), Value thisArg = Value::undefined()) {
  TRY(CheckStack());
  double targetIndex = start;
  for (double sourceIndex = 0; sourceIndex < sourceLen; sourceIndex++) {
    sourceIndex = nextPresent(source, sourceIndex, sourceLen);
    if (sourceIndex >= sourceLen) break;
    PropertyKey p = indexKey(sourceIndex);
    bool exists = TRY(HasProperty(source, p));
    if (!exists) continue;
    Value element = TRY(Get(source, p));
    if (!mapperFunction.isUndefined()) element = TRY(Call(mapperFunction, thisArg, {element, num(sourceIndex), Value(source)}));
    bool shouldFlatten = false;
    if (depth > 0) shouldFlatten = TRY(IsArray(element));
    if (shouldFlatten) {
      double newDepth = depth == INFINITY ? INFINITY : depth - 1;
      double elementLen = TRY(LengthOfArrayLike(element.asObject()));
      targetIndex = TRY(FlattenIntoArray(target, element.asObject(), elementLen, targetIndex, newDepth));
    } else {
      if (targetIndex >= kMaxSafeInteger) return ThrowTypeError("flat: result is too long");
      TRY(CreateDataPropertyOrThrow(target, indexKey(targetIndex), element));
      targetIndex++;
    }
  }
  return targetIndex;
}

// 23.1.3.13 Array.prototype.flat ( [ depth ] )
Result<Value> ArrayPrototype_flat(NativeCall& call) {
  auto [o, sourceLen] = TRY(thisArrayLike(call.thisValue));
  double depthNum = 1;
  if (!call.args[0].isUndefined()) {
    depthNum = TRY(ToIntegerOrInfinity(call.args[0]));
    if (depthNum < 0) depthNum = 0;
  }
  Object* a = TRY(ArraySpeciesCreate(o, 0));
  TRY(FlattenIntoArray(a, o, sourceLen, 0, depthNum));
  return Value(a);
}

// 23.1.3.14 Array.prototype.flatMap ( mapper [ , thisArg ] )
Result<Value> ArrayPrototype_flatMap(NativeCall& call) {
  auto [o, sourceLen] = TRY(thisArrayLike(call.thisValue));
  Value mapper = TRY(requireCallable(call.args[0], "Array.prototype.flatMap"));
  Object* a = TRY(ArraySpeciesCreate(o, 0));
  TRY(FlattenIntoArray(a, o, sourceLen, 0, 1, mapper, call.args[1]));
  return Value(a);
}

// 23.1.3.16 Array.prototype.includes ( searchElement [ , fromIndex ] )
Result<Value> ArrayPrototype_includes(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  if (len == 0) return boolean(false);
  double n = TRY(ToIntegerOrInfinity(call.args[1]));
  if (n == INFINITY) return boolean(false);
  if (n == -INFINITY) n = 0;
  double k = n >= 0 ? n : std::max(len + n, 0.0);
  bool undefinedSearch = call.args[0].isUndefined();
  for (; k < len; k++) {
    // Absent elements read as undefined, so they can only match undefined.
    if (!undefinedSearch) {
      k = nextPresent(o, k, len);
      if (k >= len) break;
    }
    Value elementK = TRY(Get(o, indexKey(k)));
    if (SameValueZero(call.args[0], elementK)) return boolean(true);
  }
  return boolean(false);
}

// 23.1.3.17 Array.prototype.indexOf ( searchElement [ , fromIndex ] )
Result<Value> ArrayPrototype_indexOf(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  if (len == 0) return num(-1);
  double n = TRY(ToIntegerOrInfinity(call.args[1]));
  if (n == INFINITY) return num(-1);
  if (n == -INFINITY) n = 0;
  double k = n >= 0 ? n : std::max(len + n, 0.0);
  for (; k < len; k++) {
    k = nextPresent(o, k, len);
    if (k >= len) break;
    PropertyKey pk = indexKey(k);
    bool kPresent = TRY(HasProperty(o, pk));
    if (!kPresent) continue;
    Value elementK = TRY(Get(o, pk));
    if (IsStrictlyEqual(call.args[0], elementK)) return num(k);
  }
  return num(-1);
}

// 23.1.3.18 Array.prototype.join ( separator )
Result<Value> ArrayPrototype_join(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  String* sep = call.args[0].isUndefined() ? atom(u",") : TRY(ToString(call.args[0]));
  std::u16string r;
  for (double k = 0; k < len; k++) {
    if (k > 0) r.append(sep->view());
    Value element = TRY(Get(o, indexKey(k)));
    if (!element.isNullish()) {
      String* next = TRY(ToString(element));
      r.append(next->view());
    }
    if (r.size() > (size_t(1) << 30)) return ThrowRangeError("Invalid string length");
  }
  return Value(newString(std::move(r)));
}

// 23.1.3.20 Array.prototype.lastIndexOf ( searchElement [ , fromIndex ] )
Result<Value> ArrayPrototype_lastIndexOf(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  if (len == 0) return num(-1);
  double n = call.args.size() > 1 ? TRY(ToIntegerOrInfinity(call.args[1])) : len - 1;
  if (n == -INFINITY) return num(-1);
  double k = n >= 0 ? std::min(n, len - 1) : len + n;
  for (; k >= 0; k--) {
    k = prevPresent(o, k);
    if (k < 0) break;
    PropertyKey pk = indexKey(k);
    bool kPresent = TRY(HasProperty(o, pk));
    if (!kPresent) continue;
    Value elementK = TRY(Get(o, pk));
    if (IsStrictlyEqual(call.args[0], elementK)) return num(k);
  }
  return num(-1);
}

// 23.1.3.21 Array.prototype.map ( callback [ , thisArg ] )
Result<Value> ArrayPrototype_map(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  Value callback = TRY(requireCallable(call.args[0], "Array.prototype.map"));
  Object* a = TRY(ArraySpeciesCreate(o, len));
  for (double k = 0; k < len; k++) {
    k = nextPresent(o, k, len);
    if (k >= len) break;
    PropertyKey pk = indexKey(k);
    bool kPresent = TRY(HasProperty(o, pk));
    if (!kPresent) continue;
    Value kValue = TRY(Get(o, pk));
    Value mappedValue = TRY(Call(callback, call.args[1], {kValue, num(k), Value(o)}));
    TRY(CreateDataPropertyOrThrow(a, pk, mappedValue));
  }
  return Value(a);
}

// 23.1.3.22 Array.prototype.pop ( )
Result<Value> ArrayPrototype_pop(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  if (len == 0) {
    TRY(setLength(o, 0));
    return Value::undefined();
  }
  double newLen = len - 1;
  PropertyKey index = indexKey(newLen);
  Value element = TRY(Get(o, index));
  TRY(DeletePropertyOrThrow(o, index));
  TRY(setLength(o, newLen));
  return element;
}

// 23.1.3.23 Array.prototype.push ( ...items )
Result<Value> ArrayPrototype_push(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  double argCount = double(call.args.size());
  if (len + argCount > kMaxSafeInteger) return ThrowTypeError("Array.prototype.push: result is too long");
  for (Value e : call.args) {
    TRY(Set(o, indexKey(len), e, true));
    len++;
  }
  TRY(setLength(o, len));
  return num(len);
}

// 23.1.3.24 / 23.1.3.25 reduce and reduceRight
Result<Value> reduce(NativeCall& call, bool right) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  Value callback = TRY(requireCallable(call.args[0], right ? "Array.prototype.reduceRight" : "Array.prototype.reduce"));
  if (len == 0 && call.args.size() < 2) return ThrowTypeError("Reduce of empty array with no initial value");
  double k = right ? len - 1 : 0;
  auto inRange = [&](double i) { return right ? i >= 0 : i < len; };
  auto skip = [&](double i) { return right ? prevPresent(o, i) : nextPresent(o, i, len); };
  double step = right ? -1 : 1;
  Value accumulator;
  if (call.args.size() >= 2) {
    accumulator = call.args[1];
  } else {
    bool kPresent = false;
    while (!kPresent && inRange(k)) {
      k = skip(k);
      if (!inRange(k)) break;
      PropertyKey pk = indexKey(k);
      kPresent = TRY(HasProperty(o, pk));
      if (kPresent) accumulator = TRY(Get(o, pk));
      k += step;
    }
    if (!kPresent) return ThrowTypeError("Reduce of empty array with no initial value");
  }
  for (; inRange(k); k += step) {
    k = skip(k);
    if (!inRange(k)) break;
    PropertyKey pk = indexKey(k);
    bool kPresent = TRY(HasProperty(o, pk));
    if (!kPresent) continue;
    Value kValue = TRY(Get(o, pk));
    accumulator = TRY(Call(callback, Value::undefined(), {accumulator, kValue, num(k), Value(o)}));
  }
  return accumulator;
}

Result<Value> ArrayPrototype_reduce(NativeCall& call) { return reduce(call, false); }
Result<Value> ArrayPrototype_reduceRight(NativeCall& call) { return reduce(call, true); }

// 23.1.3.26 Array.prototype.reverse ( )
Result<Value> ArrayPrototype_reverse(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  double middle = std::floor(len / 2);
  for (double lower = 0; lower != middle; lower++) {
    // Skip pairs where both ends are provably absent.
    if (simpleIndexed(o) && len <= kIndexLimit) {
      double nextLower = nextPresent(o, lower, middle);
      double upperCandidate = prevPresent(o, len - lower - 1);
      double nextFromUpper = upperCandidate < 0 ? middle : len - 1 - upperCandidate;
      double target = std::min(nextLower, nextFromUpper);
      if (target >= middle) break;
      lower = target;
    }
    double upper = len - lower - 1;
    PropertyKey upperP = indexKey(upper), lowerP = indexKey(lower);
    bool lowerExists = TRY(HasProperty(o, lowerP));
    Value lowerValue, upperValue;
    if (lowerExists) lowerValue = TRY(Get(o, lowerP));
    bool upperExists = TRY(HasProperty(o, upperP));
    if (upperExists) upperValue = TRY(Get(o, upperP));
    if (lowerExists && upperExists) {
      TRY(Set(o, lowerP, upperValue, true));
      TRY(Set(o, upperP, lowerValue, true));
    } else if (!lowerExists && upperExists) {
      TRY(Set(o, lowerP, upperValue, true));
      TRY(DeletePropertyOrThrow(o, upperP));
    } else if (lowerExists && !upperExists) {
      TRY(DeletePropertyOrThrow(o, lowerP));
      TRY(Set(o, upperP, lowerValue, true));
    }
  }
  return Value(o);
}

// 23.1.3.27 Array.prototype.shift ( )
Result<Value> ArrayPrototype_shift(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  if (len == 0) {
    TRY(setLength(o, 0));
    return Value::undefined();
  }
  Value first = TRY(Get(o, PropertyKey::index(0)));
  for (double k = 1; k < len; k++) {
    PropertyKey from = indexKey(k), to = indexKey(k - 1);
    bool fromPresent = TRY(HasProperty(o, from));
    if (fromPresent) {
      Value fromVal = TRY(Get(o, from));
      TRY(Set(o, to, fromVal, true));
    } else {
      TRY(DeletePropertyOrThrow(o, to));
    }
  }
  TRY(DeletePropertyOrThrow(o, indexKey(len - 1)));
  TRY(setLength(o, len - 1));
  return first;
}

// 23.1.3.28 Array.prototype.slice ( start, end )
Result<Value> ArrayPrototype_slice(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  double k = relativeIndex(TRY(ToIntegerOrInfinity(call.args[0])), len);
  double relativeEnd = call.args[1].isUndefined() ? len : TRY(ToIntegerOrInfinity(call.args[1]));
  double final = relativeIndex(relativeEnd, len);
  double count = std::max(final - k, 0.0);
  Object* a = TRY(ArraySpeciesCreate(o, count));
  double n = 0;
  for (; k < final; k++, n++) {
    double next = nextPresent(o, k, final);
    n += next - k;
    k = next;
    if (k >= final) break;
    PropertyKey pk = indexKey(k);
    bool kPresent = TRY(HasProperty(o, pk));
    if (kPresent) {
      Value kValue = TRY(Get(o, pk));
      TRY(CreateDataPropertyOrThrow(a, indexKey(n), kValue));
    }
  }
  TRY(setLength(a, n));
  return Value(a);
}

// 23.1.3.30.2 CompareArrayElements ( x, y, comparator )
Result<double> CompareArrayElements(Value x, Value y, Value comparator) {
  if (x.isUndefined() && y.isUndefined()) return 0.0;
  if (x.isUndefined()) return 1.0;
  if (y.isUndefined()) return -1.0;
  if (!comparator.isUndefined()) {
    Value r = TRY(Call(comparator, Value::undefined(), {x, y}));
    double v = TRY(ToNumber(r));
    if (std::isnan(v)) return 0.0;
    return v;
  }
  String* xString = TRY(ToString(x));
  String* yString = TRY(ToString(y));
  if (xString->view() < yString->view()) return -1.0;
  if (yString->view() < xString->view()) return 1.0;
  return 0.0;
}

// A stable merge sort whose comparator may throw.
Result<void> mergeSort(std::vector<Value>& items, Value comparator) {
  size_t n = items.size();
  ValueVector buffer;
  buffer.resize(n);
  for (size_t width = 1; width < n; width *= 2) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      size_t mid = std::min(lo + width, n), hi = std::min(lo + 2 * width, n);
      size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        double c = TRY(CompareArrayElements(items[j], items[i], comparator));
        if (c < 0) buffer[k++] = items[j++];
        else buffer[k++] = items[i++];
      }
      while (i < mid) buffer[k++] = items[i++];
      while (j < hi) buffer[k++] = items[j++];
    }
    for (size_t i = 0; i < n; i++) items[i] = buffer[i];
  }
  return {};
}

// 23.1.3.30.1 SortIndexedProperties ( obj, len, SortCompare, holes )
Result<ValueVector> SortIndexedProperties(Object* obj, double len, Value comparator, bool skipHoles) {
  ValueVector items;
  for (double k = 0; k < len; k++) {
    if (skipHoles) {
      k = nextPresent(obj, k, len);
      if (k >= len) break;
    }
    PropertyKey pk = indexKey(k);
    bool kRead = skipHoles ? TRY(HasProperty(obj, pk)) : true;
    if (kRead) items.push_back(TRY(Get(obj, pk)));
  }
  TRY(mergeSort(items.vector(), comparator));
  return items;
}

// 23.1.3.30 Array.prototype.sort ( comparator )
Result<Value> ArrayPrototype_sort(NativeCall& call) {
  Value comparator = call.args[0];
  if (!comparator.isUndefined() && !IsCallable(comparator)) return ThrowTypeError("The comparison function must be either a function or undefined");
  auto [obj, len] = TRY(thisArrayLike(call.thisValue));
  ValueVector sortedList = TRY(SortIndexedProperties(obj, len, comparator, true));
  double itemCount = double(sortedList.size());
  double j = 0;
  for (; j < itemCount; j++) TRY(Set(obj, indexKey(j), sortedList[size_t(j)], true));
  for (; j < len; j++) {
    j = nextPresent(obj, j, len);
    if (j >= len) break;
    TRY(DeletePropertyOrThrow(obj, indexKey(j)));
  }
  return Value(obj);
}

// 23.1.3.31 Array.prototype.splice ( start, deleteCount, ...items )
Result<Value> ArrayPrototype_splice(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  double actualStart = relativeIndex(TRY(ToIntegerOrInfinity(call.args[0])), len);
  Args items = call.args.slice(2);
  double itemCount = double(items.size());
  double actualDeleteCount;
  if (call.args.size() == 0) {
    actualDeleteCount = 0;
  } else if (call.args.size() == 1) {
    actualDeleteCount = len - actualStart;
  } else {
    double dc = TRY(ToIntegerOrInfinity(call.args[1]));
    actualDeleteCount = std::min(std::max(dc, 0.0), len - actualStart);
  }
  if (len + itemCount - actualDeleteCount > kMaxSafeInteger) return ThrowTypeError("Array.prototype.splice: result is too long");
  Object* a = TRY(ArraySpeciesCreate(o, actualDeleteCount));
  for (double k = 0; k < actualDeleteCount; k++) {
    PropertyKey from = indexKey(actualStart + k);
    bool fromPresent = TRY(HasProperty(o, from));
    if (fromPresent) {
      Value fromValue = TRY(Get(o, from));
      TRY(CreateDataPropertyOrThrow(a, indexKey(k), fromValue));
    }
  }
  TRY(setLength(a, actualDeleteCount));
  if (itemCount < actualDeleteCount) {
    for (double k = actualStart; k < len - actualDeleteCount; k++) {
      PropertyKey from = indexKey(k + actualDeleteCount), to = indexKey(k + itemCount);
      bool fromPresent = TRY(HasProperty(o, from));
      if (fromPresent) {
        Value fromValue = TRY(Get(o, from));
        TRY(Set(o, to, fromValue, true));
      } else {
        TRY(DeletePropertyOrThrow(o, to));
      }
    }
    for (double k = len; k > len - actualDeleteCount + itemCount; k--) TRY(DeletePropertyOrThrow(o, indexKey(k - 1)));
  } else if (itemCount > actualDeleteCount) {
    for (double k = len - actualDeleteCount; k > actualStart; k--) {
      PropertyKey from = indexKey(k + actualDeleteCount - 1), to = indexKey(k + itemCount - 1);
      bool fromPresent = TRY(HasProperty(o, from));
      if (fromPresent) {
        Value fromValue = TRY(Get(o, from));
        TRY(Set(o, to, fromValue, true));
      } else {
        TRY(DeletePropertyOrThrow(o, to));
      }
    }
  }
  double k = actualStart;
  for (Value e : items) {
    TRY(Set(o, indexKey(k), e, true));
    k++;
  }
  TRY(setLength(o, len - actualDeleteCount + itemCount));
  return Value(a);
}

// 23.1.3.32 Array.prototype.toLocaleString ( [ reserved1 [ , reserved2 ] ] )
Result<Value> ArrayPrototype_toLocaleString(NativeCall& call) {
  auto [array, len] = TRY(thisArrayLike(call.thisValue));
  std::u16string r;
  for (double k = 0; k < len; k++) {
    if (k > 0) r.push_back(u',');
    Value nextElement = TRY(Get(array, indexKey(k)));
    if (!nextElement.isNullish()) {
      Value s = TRY(Invoke(nextElement, PropertyKey(u"toLocaleString")));
      String* str = TRY(ToString(s));
      r.append(str->view());
    }
  }
  return Value(newString(std::move(r)));
}

// 23.1.3.33 Array.prototype.toReversed ( )
Result<Value> ArrayPrototype_toReversed(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  Object* a = TRY(ArrayCreate(len));
  for (double k = 0; k < len; k++) {
    Value fromValue = TRY(Get(o, indexKey(len - k - 1)));
    TRY(CreateDataPropertyOrThrow(a, indexKey(k), fromValue));
  }
  return Value(a);
}

// 23.1.3.34 Array.prototype.toSorted ( comparator )
Result<Value> ArrayPrototype_toSorted(NativeCall& call) {
  Value comparator = call.args[0];
  if (!comparator.isUndefined() && !IsCallable(comparator)) return ThrowTypeError("The comparison function must be either a function or undefined");
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  Object* a = TRY(ArrayCreate(len));
  ValueVector sortedList = TRY(SortIndexedProperties(o, len, comparator, false));
  for (size_t j = 0; j < sortedList.size(); j++) TRY(CreateDataPropertyOrThrow(a, indexKey(double(j)), sortedList[j]));
  return Value(a);
}

// 23.1.3.35 Array.prototype.toSpliced ( start, skipCount, ...items )
Result<Value> ArrayPrototype_toSpliced(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  double actualStart = relativeIndex(TRY(ToIntegerOrInfinity(call.args[0])), len);
  Args items = call.args.slice(2);
  double insertCount = double(items.size());
  double actualSkipCount;
  if (call.args.size() == 0) {
    actualSkipCount = 0;
  } else if (call.args.size() == 1) {
    actualSkipCount = len - actualStart;
  } else {
    double sc = TRY(ToIntegerOrInfinity(call.args[1]));
    actualSkipCount = std::min(std::max(sc, 0.0), len - actualStart);
  }
  double newLen = len + insertCount - actualSkipCount;
  if (newLen > kMaxSafeInteger) return ThrowTypeError("Array.prototype.toSpliced: result is too long");
  Object* a = TRY(ArrayCreate(newLen));
  double i = 0, r = actualStart + actualSkipCount;
  for (; i < actualStart; i++) {
    PropertyKey pi = indexKey(i);
    Value iValue = TRY(Get(o, pi));
    TRY(CreateDataPropertyOrThrow(a, pi, iValue));
  }
  for (Value e : items) {
    TRY(CreateDataPropertyOrThrow(a, indexKey(i), e));
    i++;
  }
  for (; i < newLen; i++, r++) {
    Value fromValue = TRY(Get(o, indexKey(r)));
    TRY(CreateDataPropertyOrThrow(a, indexKey(i), fromValue));
  }
  return Value(a);
}

// 23.1.3.36 Array.prototype.toString ( )
Result<Value> ArrayPrototype_toString(NativeCall& call) {
  Object* array = TRY(ToObject(call.thisValue));
  Value func = TRY(Get(array, PropertyKey(u"join")));
  if (!IsCallable(func)) {
    NativeCall inner{Value(array), Args(), nullptr, call.callee};
    return ObjectPrototype_toString(inner);
  }
  return Call(func, array);
}

// 23.1.3.37 Array.prototype.unshift ( ...items )
Result<Value> ArrayPrototype_unshift(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  double argCount = double(call.args.size());
  if (argCount > 0) {
    if (len + argCount > kMaxSafeInteger) return ThrowTypeError("Array.prototype.unshift: result is too long");
    for (double k = len; k > 0; k--) {
      PropertyKey from = indexKey(k - 1), to = indexKey(k + argCount - 1);
      bool fromPresent = TRY(HasProperty(o, from));
      if (fromPresent) {
        Value fromValue = TRY(Get(o, from));
        TRY(Set(o, to, fromValue, true));
      } else {
        TRY(DeletePropertyOrThrow(o, to));
      }
    }
    double j = 0;
    for (Value e : call.args) {
      TRY(Set(o, indexKey(j), e, true));
      j++;
    }
  }
  TRY(setLength(o, len + argCount));
  return num(len + argCount);
}

// 23.1.3.39 Array.prototype.with ( index, value )
Result<Value> ArrayPrototype_with(NativeCall& call) {
  auto [o, len] = TRY(thisArrayLike(call.thisValue));
  double relativeIndex = TRY(ToIntegerOrInfinity(call.args[0]));
  double actualIndex = relativeIndex >= 0 ? relativeIndex : len + relativeIndex;
  if (actualIndex >= len || actualIndex < 0) return ThrowRangeError("Invalid index");
  Object* a = TRY(ArrayCreate(len));
  for (double k = 0; k < len; k++) {
    PropertyKey pk = indexKey(k);
    Value fromValue = k == actualIndex ? call.args[1] : TRY(Get(o, pk));
    TRY(CreateDataPropertyOrThrow(a, pk, fromValue));
  }
  return Value(a);
}

void installArray(Realm& realm) {
  Object* proto = make<ArrayObject>(realm.intrinsic(Intrinsic::ObjectPrototype), 0);
  BuiltinFunction* ctor =
      defineConstructor(realm, Intrinsic::Array, Array_construct, 1, PropertyKey(u"Array"), proto, Intrinsic::ArrayPrototype);
  defineFunction(realm, ctor, PropertyKey(u"from"), 1, Array_from);
  defineFunction(realm, ctor, PropertyKey(u"isArray"), 1, Array_isArray);
  defineFunction(realm, ctor, PropertyKey(u"of"), 0, Array_of);
  defineAccessor(realm, ctor, PropertyKey(symbols().species), Array_species, nullptr);

  struct Fn {
    const char16_t* name;
    int length;
    NativeFn fn;
  };
  const Fn fns[] = {
      {u"at", 1, ArrayPrototype_at},
      {u"concat", 1, ArrayPrototype_concat},
      {u"copyWithin", 2, ArrayPrototype_copyWithin},
      {u"entries", 0, ArrayPrototype_entries},
      {u"every", 1, ArrayPrototype_every},
      {u"fill", 1, ArrayPrototype_fill},
      {u"filter", 1, ArrayPrototype_filter},
      {u"find", 1, ArrayPrototype_find},
      {u"findIndex", 1, ArrayPrototype_findIndex},
      {u"findLast", 1, ArrayPrototype_findLast},
      {u"findLastIndex", 1, ArrayPrototype_findLastIndex},
      {u"flat", 0, ArrayPrototype_flat},
      {u"flatMap", 1, ArrayPrototype_flatMap},
      {u"forEach", 1, ArrayPrototype_forEach},
      {u"includes", 1, ArrayPrototype_includes},
      {u"indexOf", 1, ArrayPrototype_indexOf},
      {u"join", 1, ArrayPrototype_join},
      {u"keys", 0, ArrayPrototype_keys},
      {u"lastIndexOf", 1, ArrayPrototype_lastIndexOf},
      {u"map", 1, ArrayPrototype_map},
      {u"pop", 0, ArrayPrototype_pop},
      {u"push", 1, ArrayPrototype_push},
      {u"reduce", 1, ArrayPrototype_reduce},
      {u"reduceRight", 1, ArrayPrototype_reduceRight},
      {u"reverse", 0, ArrayPrototype_reverse},
      {u"shift", 0, ArrayPrototype_shift},
      {u"slice", 2, ArrayPrototype_slice},
      {u"some", 1, ArrayPrototype_some},
      {u"sort", 1, ArrayPrototype_sort},
      {u"splice", 2, ArrayPrototype_splice},
      {u"toLocaleString", 0, ArrayPrototype_toLocaleString},
      {u"toReversed", 0, ArrayPrototype_toReversed},
      {u"toSorted", 1, ArrayPrototype_toSorted},
      {u"toSpliced", 2, ArrayPrototype_toSpliced},
      {u"toString", 0, ArrayPrototype_toString},
      {u"unshift", 1, ArrayPrototype_unshift},
  };
  for (const Fn& f : fns) defineFunction(realm, proto, PropertyKey(f.name), f.length, f.fn);
  BuiltinFunction* values = defineFunction(realm, proto, PropertyKey(u"values"), 0, ArrayPrototype_values);
  defineFunction(realm, proto, PropertyKey(u"with"), 2, ArrayPrototype_with);
  realm.setIntrinsic(Intrinsic::ArrayPrototypeValues, values);
  defineValue(proto, PropertyKey(symbols().iterator), values);

  Object* unscopables = OrdinaryObjectCreate(nullptr);
  for (const char16_t* name : {u"at", u"copyWithin", u"entries", u"fill", u"find", u"findIndex", u"findLast",
                               u"findLastIndex", u"flat", u"flatMap", u"includes", u"keys", u"toReversed",
                               u"toSorted", u"toSpliced", u"values"}) {
    defineValue(unscopables, PropertyKey(name), boolean(true), Attr::All);
  }
  defineValue(proto, PropertyKey(symbols().unscopables), unscopables, Attr::Configurable);

  Object* iterProto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::IteratorPrototype));
  realm.setIntrinsic(Intrinsic::ArrayIteratorPrototype, iterProto);
  defineFunction(realm, iterProto, PropertyKey(u"next"), 0, ArrayIteratorPrototype_next);
  defineValue(iterProto, PropertyKey(symbols().toStringTag), str(u"Array Iterator"), Attr::Configurable);
}

}  // namespace

// Exposed for %TypedArray%.prototype's keys/values/entries.
Object* CreateArrayIteratorForKind(Object* array, int kind) { return CreateArrayIterator(array, IterKind(kind)); }

SNOWMAN_BUILTIN(Array, kOrderDefault, installArray);

}  // namespace snowman::builtins
