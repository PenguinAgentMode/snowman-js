// Abstract operations, named and parameterised as in ECMA-262 (7.1–7.4, 10.1, 10.4). INTERFACES.md section 6.
// Spec integers that fit in 2^53 are doubles here. Add operations under their spec names.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "runtime/errors.h"
#include "runtime/function.h"
#include "runtime/object.h"
#include "runtime/property.h"
#include "runtime/realm.h"
#include "runtime/result.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace snowman {

// 7.1 Type conversion
enum class PreferredType { None, String, Number };
Result<Value> ToPrimitive(Value input, PreferredType preferredType = PreferredType::None);
Result<Value> OrdinaryToPrimitive(Object* object, PreferredType hint);
bool ToBoolean(Value argument);
Result<Value> ToNumeric(Value value);  // a Number or a BigInt
Result<double> ToNumber(Value argument);
double StringToNumber(std::u16string_view text);
Result<double> ToIntegerOrInfinity(Value argument);
Result<int32_t> ToInt32(Value argument);
Result<uint32_t> ToUint32(Value argument);
Result<int16_t> ToInt16(Value argument);
Result<uint16_t> ToUint16(Value argument);
Result<int8_t> ToInt8(Value argument);
Result<uint8_t> ToUint8(Value argument);
Result<uint8_t> ToUint8Clamp(Value argument);
Result<BigInt*> ToBigInt(Value argument);
Result<int64_t> ToBigInt64(Value argument);
Result<uint64_t> ToBigUint64(Value argument);
Result<String*> ToString(Value argument);
Result<Object*> ToObject(Value argument);
Result<PropertyKey> ToPropertyKey(Value argument);
Result<double> ToLength(Value argument);
Result<double> ToIndex(Value value);
// CanonicalNumericIndexString: the Number, or nullopt for undefined.
std::optional<double> CanonicalNumericIndexString(String* argument);
// Number::toString(x, radix) (shortest round-trip for radix 10).
String* NumberToString(double x, int radix = 10);
// The pure numeric conversions, for callers that already have a double.
int32_t DoubleToInt32(double d);
uint32_t DoubleToUint32(double d);

// 7.2 Testing and comparison
Result<void> RequireObjectCoercible(Value argument);
Result<bool> IsArray(Value argument);
bool IsCallable(Value argument);
bool IsConstructor(Value argument);
bool IsIntegralNumber(Value argument);
bool IsRegExpLike(Value argument);  // [[RegExpMatcher]] check only; IsRegExp (Symbol.match) is Result<bool>
Result<bool> IsRegExp(Value argument);
bool SameValue(Value x, Value y);
bool SameValueZero(Value x, Value y);
bool SameValueNonNumber(Value x, Value y);
bool IsStrictlyEqual(Value x, Value y);
Result<bool> IsLooselyEqual(Value x, Value y);
// IsLessThan: true, false, or undefined (nullopt) when a NaN is involved.
Result<std::optional<bool>> IsLessThan(Value x, Value y, bool leftFirst);

// 7.3 Operations on objects
Result<bool> IsExtensible(Object* object);
Result<Value> Get(Object* object, const PropertyKey& key);
Result<Value> GetV(Value value, const PropertyKey& key);
Result<void> Set(Object* object, const PropertyKey& key, Value value, bool throwOnFailure);
Result<bool> CreateDataProperty(Object* object, const PropertyKey& key, Value value);
Result<void> CreateDataPropertyOrThrow(Object* object, const PropertyKey& key, Value value);
Result<void> CreateNonEnumerableDataPropertyOrThrow(Object* object, const PropertyKey& key, Value value);
Result<void> DefinePropertyOrThrow(Object* object, const PropertyKey& key, const PropertyDescriptor& desc);
Result<void> DeletePropertyOrThrow(Object* object, const PropertyKey& key);
Result<Value> GetMethod(Value value, const PropertyKey& key);  // undefined or a callable
Result<bool> HasProperty(Object* object, const PropertyKey& key);
Result<bool> HasOwnProperty(Object* object, const PropertyKey& key);
Result<Value> Call(Value function, Value thisValue, Args args = {});
Result<Object*> Construct(Object* constructor, Args args = {}, Object* newTarget = nullptr);
enum class IntegrityLevel { Sealed, Frozen };
Result<bool> SetIntegrityLevel(Object* object, IntegrityLevel level);
Result<bool> TestIntegrityLevel(Object* object, IntegrityLevel level);
Object* CreateArrayFromList(Args elements);
Result<double> LengthOfArrayLike(Object* object);
// elementTypes: restrict to String/Symbol for Reflect.ownKeys-like callers (CreateListFromArrayLike's
// optional argument); empty = all types.
Result<ValueVector> CreateListFromArrayLike(Value object, bool onlyPropertyKeys = false);
Result<Value> Invoke(Value value, const PropertyKey& key, Args args = {});
Result<bool> OrdinaryHasInstance(Value constructor, Value object);
Result<bool> InstanceofOperator(Value value, Value target);
Result<Object*> SpeciesConstructor(Object* object, Object* defaultConstructor);
enum class EnumerableOwnKind { Key, Value, KeyValue };
Result<ValueVector> EnumerableOwnProperties(Object* object, EnumerableOwnKind kind);
Result<Realm*> GetFunctionRealm(Object* object);
Result<void> CopyDataProperties(Object* target, Value source, const std::vector<PropertyKey>& excludedKeys);

// 10.1 Ordinary objects
Result<Object*> GetPrototypeFromConstructor(Object* constructor, Intrinsic intrinsicDefaultProto);
// OrdinaryCreateFromConstructor: T's constructor takes the prototype first, then `args`.
template <class T, class... A>
Result<T*> OrdinaryCreateFromConstructor(Object* constructor, Intrinsic intrinsicDefaultProto, A&&... args) {
  Object* prototype = TRY(GetPrototypeFromConstructor(constructor, intrinsicDefaultProto));
  return make<T>(prototype, std::forward<A>(args)...);
}
Object* OrdinaryObjectCreate(Object* prototype);
bool IsCompatiblePropertyDescriptor(bool extensible, const PropertyDescriptor& desc,
                                    const std::optional<PropertyDescriptor>& current);
// O may be nullptr (validation only), as in the spec.
bool ValidateAndApplyPropertyDescriptor(Object* object, const PropertyKey& key, bool extensible,
                                        const PropertyDescriptor& desc,
                                        const std::optional<PropertyDescriptor>& current);
Result<PropertyDescriptor> ToPropertyDescriptor(Value object);
Value FromPropertyDescriptor(const std::optional<PropertyDescriptor>& desc);
void CompletePropertyDescriptor(PropertyDescriptor& desc);

// 10.4 Built-in exotic objects
Result<Object*> ArrayCreate(double length, Object* prototype = nullptr);  // nullptr: %Array.prototype%
Result<Object*> ArraySpeciesCreate(Object* originalArray, double length);

// 7.4 Iterators (the sync ones; the async ones need Await and live in src/interp/)
struct IteratorRecord {
  Object* iterator = nullptr;
  Value nextMethod;
  bool done = false;
};
enum class IteratorKind { Sync, Async };
Result<IteratorRecord> GetIteratorFromMethod(Value object, Value method);
Result<IteratorRecord> GetIterator(Value object, IteratorKind kind);
Result<IteratorRecord> GetIteratorDirect(Object* object);
Result<Object*> IteratorNext(IteratorRecord& record, std::optional<Value> value = std::nullopt);
Result<bool> IteratorComplete(Object* iteratorResult);
Result<Value> IteratorValue(Object* iteratorResult);
// IteratorStep: the result object, or nullptr when done.
Result<Object*> IteratorStep(IteratorRecord& record);
// IteratorStepValue: the value, or nullopt when done.
Result<std::optional<Value>> IteratorStepValue(IteratorRecord& record);
// IteratorClose(iteratorRecord, completion): pass the pending completion (normal: {}; throw: Throw{v}).
Result<void> IteratorClose(const IteratorRecord& record, Result<void> completion);
Object* CreateIteratorResultObject(Value value, bool done);
Result<ValueVector> IteratorToList(IteratorRecord& record);

}  // namespace snowman
