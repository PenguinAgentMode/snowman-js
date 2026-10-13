// Realms and the %Intrinsic% table. INTERFACES.md section 8.
#pragma once

#include <cstddef>
#include <cstdint>

#include "runtime/object.h"

namespace snowman {

// The well-known intrinsics (ECMA-262 Table 6), named %Name% → Name and %Name.prototype% → NamePrototype.
// Add entries here (append-only within each line is not required; keep alphabetical order).
#define SNOWMAN_INTRINSICS(X)                                                                                   \
  X(AggregateError) X(AggregateErrorPrototype) X(Array) X(ArrayPrototype) X(ArrayPrototypeValues)               \
  X(ArrayBuffer) X(ArrayBufferPrototype) X(ArrayIteratorPrototype) X(AsyncFromSyncIteratorPrototype)            \
  X(AsyncDisposableStack) X(AsyncDisposableStackPrototype) X(DisposableStack) X(DisposableStackPrototype)       \
  X(AsyncFunction) X(AsyncFunctionPrototype) X(AsyncGeneratorFunction) X(AsyncGeneratorFunctionPrototype)       \
  X(AsyncGeneratorPrototype) X(AsyncIteratorPrototype) X(Atomics) X(BigInt) X(BigIntPrototype)                  \
  X(BigInt64Array) X(BigInt64ArrayPrototype) X(BigUint64Array) X(BigUint64ArrayPrototype) X(Boolean)            \
  X(BooleanPrototype) X(DataView) X(DataViewPrototype) X(Date) X(DatePrototype) X(decodeURI)                    \
  X(decodeURIComponent) X(encodeURI) X(encodeURIComponent) X(Error) X(ErrorPrototype) X(escape) X(eval)        \
  X(EvalError) X(EvalErrorPrototype) X(FinalizationRegistry) X(FinalizationRegistryPrototype) X(Float16Array)   \
  X(Float16ArrayPrototype) X(Float32Array) X(Float32ArrayPrototype) X(Float64Array) X(Float64ArrayPrototype)    \
  X(ForInIteratorPrototype) X(Function) X(FunctionPrototype) X(GeneratorFunction) X(GeneratorFunctionPrototype) \
  X(GeneratorPrototype) X(Int8Array) X(Int8ArrayPrototype) X(Int16Array) X(Int16ArrayPrototype) X(Int32Array)   \
  X(Int32ArrayPrototype) X(isFinite) X(isNaN) X(Iterator) X(IteratorPrototype) X(IteratorHelperPrototype)       \
  X(JSON) X(Map) X(MapPrototype) X(MapIteratorPrototype) X(Math) X(Number) X(NumberPrototype) X(Object)         \
  X(ObjectPrototype) X(parseFloat) X(parseInt) X(Promise) X(PromisePrototype) X(Proxy) X(RangeError)            \
  X(RangeErrorPrototype) X(ReferenceError) X(ReferenceErrorPrototype) X(Reflect) X(RegExp) X(RegExpPrototype)   \
  X(RegExpStringIteratorPrototype) X(Set) X(SetPrototype) X(SetIteratorPrototype) X(ShadowRealm)            \
  X(ShadowRealmPrototype) X(SharedArrayBuffer)          \
  X(SharedArrayBufferPrototype) X(String) X(StringPrototype) X(StringIteratorPrototype) X(Symbol)               \
  X(SymbolPrototype) X(SuppressedError) X(SuppressedErrorPrototype) X(SyntaxError) X(SyntaxErrorPrototype) X(Temporal) X(TemporalDuration) X(TemporalDurationPrototype)                \
  X(TemporalInstant) X(TemporalInstantPrototype) X(TemporalNow) X(TemporalPlainDate) X(TemporalPlainDatePrototype)  \
  X(TemporalPlainDateTime) X(TemporalPlainDateTimePrototype) X(TemporalPlainMonthDay)                              \
  X(TemporalPlainMonthDayPrototype) X(TemporalPlainTime) X(TemporalPlainTimePrototype) X(TemporalPlainYearMonth)    \
  X(TemporalPlainYearMonthPrototype) X(TemporalZonedDateTime) X(TemporalZonedDateTimePrototype) X(ThrowTypeError) X(TypedArray)                     \
  X(TypedArrayPrototype) X(TypeError) X(TypeErrorPrototype) X(Uint8Array) X(Uint8ArrayPrototype)                \
  X(Uint8ClampedArray) X(Uint8ClampedArrayPrototype) X(Uint16Array) X(Uint16ArrayPrototype) X(Uint32Array)      \
  X(Uint32ArrayPrototype) X(unescape) X(URIError) X(URIErrorPrototype) X(WeakMap) X(WeakMapPrototype)          \
  X(WeakRef) X(WeakRefPrototype) X(WeakSet) X(WeakSetPrototype) X(WrapForValidIteratorPrototype)

enum class Intrinsic : uint16_t {
#define SNOWMAN_INTRINSIC_ENUM(name) name,
  SNOWMAN_INTRINSICS(SNOWMAN_INTRINSIC_ENUM)
#undef SNOWMAN_INTRINSIC_ENUM
  Count
};

class Realm final : public Cell {
 public:
  Realm();

  Object* intrinsic(Intrinsic id) const { return intrinsics_[size_t(id)]; }
  void setIntrinsic(Intrinsic id, Object* object) { intrinsics_[size_t(id)] = object; }

  Object* globalObject() const { return globalObject_; }
  void setGlobalObject(Object* global) { globalObject_ = global; }

  // The realm's [[GlobalEnv]] (a GlobalEnvironment of src/interp/) and the interpreter's per-realm state
  // ([[TemplateMap]], [[LoadedModules]], ...). Opaque to the runtime; traced.
  Cell* globalEnv = nullptr;
  Cell* hostDefined = nullptr;

  void trace(Tracer& tracer) override;

 private:
  Object* intrinsics_[size_t(Intrinsic::Count)] = {};
  Object* globalObject_ = nullptr;
};

// CreateRealm + SetRealmGlobalObject + SetDefaultGlobalBindings: every intrinsic, and an ordinary global
// object carrying the global properties. The interpreter adds the global environment and host bindings.
Realm* CreateRealm();

}  // namespace snowman
