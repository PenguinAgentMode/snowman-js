// DataView (25.3).
#include "builtins/util.h"
#include "runtime/arraybuffer.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

// 25.3.1.3 IsViewOutOfBounds, 25.3.1.2 GetViewByteLength
bool IsViewOutOfBounds(const DataViewObject* view) {
  if (view->buffer->isDetached()) return true;
  double bufferByteLength = double(view->buffer->byteLength());
  double byteOffsetStart = view->byteOffset;
  double byteOffsetEnd = view->byteLength ? byteOffsetStart + *view->byteLength : bufferByteLength;
  return byteOffsetStart > bufferByteLength || byteOffsetEnd > bufferByteLength;
}

double GetViewByteLength(const DataViewObject* view) {
  if (view->byteLength) return *view->byteLength;
  return double(view->buffer->byteLength()) - view->byteOffset;
}

// 25.3.2.1 DataView ( buffer [ , byteOffset [ , byteLength ] ] )
Result<Value> DataView_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor DataView requires 'new'");
  ArrayBufferObject* buffer = call.args[0].isObject() ? call.args[0].asObject()->as<ArrayBufferObject>() : nullptr;
  if (!buffer) return ThrowTypeError("First argument to DataView constructor must be an ArrayBuffer");
  double offset = TRY(ToIndex(call.args[1]));
  if (buffer->isDetached()) return ThrowTypeError("DataView: buffer is detached");
  double bufferByteLength = double(buffer->byteLength());
  if (offset > bufferByteLength) return ThrowRangeError("Start offset is outside the bounds of the buffer");
  bool bufferIsFixedLength = !buffer->isResizable();
  std::optional<double> viewByteLength;
  if (call.args[2].isUndefined()) {
    if (bufferIsFixedLength) viewByteLength = bufferByteLength - offset;
  } else {
    viewByteLength = TRY(ToIndex(call.args[2]));
    if (offset + *viewByteLength > bufferByteLength) return ThrowRangeError("Invalid DataView length");
  }
  DataViewObject* o = TRY(OrdinaryCreateFromConstructor<DataViewObject>(call.newTarget, Intrinsic::DataViewPrototype));
  if (buffer->isDetached()) return ThrowTypeError("DataView: buffer is detached");
  bufferByteLength = double(buffer->byteLength());
  if (offset > bufferByteLength) return ThrowRangeError("Start offset is outside the bounds of the buffer");
  if (!call.args[2].isUndefined() && offset + *viewByteLength > bufferByteLength)
    return ThrowRangeError("Invalid DataView length");
  o->buffer = buffer;
  o->byteLength = viewByteLength;
  o->byteOffset = offset;
  return Value(o);
}

Result<DataViewObject*> thisView(Value v) {
  DataViewObject* d = v.isObject() ? v.asObject()->as<DataViewObject>() : nullptr;
  if (!d) return ThrowTypeError("DataView method called on incompatible receiver");
  return d;
}

Result<Value> DataViewPrototype_buffer(NativeCall& call) {
  DataViewObject* d = TRY(thisView(call.thisValue));
  return Value(d->buffer);
}

Result<Value> DataViewPrototype_byteLength(NativeCall& call) {
  DataViewObject* d = TRY(thisView(call.thisValue));
  if (IsViewOutOfBounds(d)) return ThrowTypeError("DataView is out of bounds");
  return num(GetViewByteLength(d));
}

Result<Value> DataViewPrototype_byteOffset(NativeCall& call) {
  DataViewObject* d = TRY(thisView(call.thisValue));
  if (IsViewOutOfBounds(d)) return ThrowTypeError("DataView is out of bounds");
  return num(d->byteOffset);
}

// 25.3.1.5 GetViewValue ( view, requestIndex, isLittleEndian, type )
template <ElementType T>
Result<Value> getViewValue(NativeCall& call) {
  DataViewObject* view = TRY(thisView(call.thisValue));
  double getIndex = TRY(ToIndex(call.args[0]));
  bool isLittleEndian = ToBoolean(call.args[1]);
  if (IsViewOutOfBounds(view)) return ThrowTypeError("DataView is out of bounds");
  double viewSize = GetViewByteLength(view);
  if (getIndex + double(ElementSize(T)) > viewSize) return ThrowRangeError("Offset is outside the bounds of the DataView");
  return GetValueFromBuffer(view->buffer, size_t(getIndex + view->byteOffset), T, isLittleEndian);
}

// 25.3.1.6 SetViewValue ( view, requestIndex, isLittleEndian, type, value )
template <ElementType T>
Result<Value> setViewValue(NativeCall& call) {
  DataViewObject* view = TRY(thisView(call.thisValue));
  double getIndex = TRY(ToIndex(call.args[0]));
  Value numberValue = TRY(ToElementValue(T, call.args[1]));
  bool isLittleEndian = ToBoolean(call.args[2]);
  if (IsViewOutOfBounds(view)) return ThrowTypeError("DataView is out of bounds");
  double viewSize = GetViewByteLength(view);
  if (getIndex + double(ElementSize(T)) > viewSize) return ThrowRangeError("Offset is outside the bounds of the DataView");
  SetValueInBuffer(view->buffer, size_t(getIndex + view->byteOffset), T, numberValue, isLittleEndian);
  return Value::undefined();
}

void installDataView(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  defineConstructor(realm, Intrinsic::DataView, DataView_construct, 1, PropertyKey(u"DataView"), proto, Intrinsic::DataViewPrototype);
  defineAccessor(realm, proto, PropertyKey(u"buffer"), DataViewPrototype_buffer, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"byteLength"), DataViewPrototype_byteLength, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"byteOffset"), DataViewPrototype_byteOffset, nullptr);
  struct Fn {
    const char16_t* name;
    NativeFn get;
    NativeFn set;
  };
  const Fn fns[] = {
      {u"BigInt64", getViewValue<ElementType::BigInt64>, setViewValue<ElementType::BigInt64>},
      {u"BigUint64", getViewValue<ElementType::BigUint64>, setViewValue<ElementType::BigUint64>},
      {u"Float16", getViewValue<ElementType::Float16>, setViewValue<ElementType::Float16>},
      {u"Float32", getViewValue<ElementType::Float32>, setViewValue<ElementType::Float32>},
      {u"Float64", getViewValue<ElementType::Float64>, setViewValue<ElementType::Float64>},
      {u"Int8", getViewValue<ElementType::Int8>, setViewValue<ElementType::Int8>},
      {u"Int16", getViewValue<ElementType::Int16>, setViewValue<ElementType::Int16>},
      {u"Int32", getViewValue<ElementType::Int32>, setViewValue<ElementType::Int32>},
      {u"Uint8", getViewValue<ElementType::Uint8>, setViewValue<ElementType::Uint8>},
      {u"Uint16", getViewValue<ElementType::Uint16>, setViewValue<ElementType::Uint16>},
      {u"Uint32", getViewValue<ElementType::Uint32>, setViewValue<ElementType::Uint32>},
  };
  for (const Fn& f : fns) {
    defineFunction(realm, proto, PropertyKey(std::u16string(u"get") + f.name), 1, f.get);
    defineFunction(realm, proto, PropertyKey(std::u16string(u"set") + f.name), 2, f.set);
  }
  defineValue(proto, PropertyKey(symbols().toStringTag), str(u"DataView"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(DataView, kOrderDefault, installDataView);

}  // namespace snowman::builtins
