// Function objects and built-in functions (10.3).
#include "runtime/function.h"

#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/realm.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/vm.h"

namespace snowman {

void FunctionObject::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(reinterpret_cast<Cell*>(realm_));
}

BuiltinFunction::BuiltinFunction(Realm* realm, NativeFn fn, Object* prototype, bool isConstructor)
    : FunctionObject(prototype, realm), fn_(fn), constructor_(isConstructor) {}

// 10.3.1 [[Call]]
Result<Value> BuiltinFunction::Call(Value thisArgument, Args args) {
  TRY(CheckStack());
  ExecutionContext context;
  context.realm = realm_;
  context.function = this;
  VM& v = vm();
  v.pushContext(&context);
  NativeCall call{thisArgument, args, nullptr, this};
  Result<Value> result = fn_(call);
  v.popContext();
  return result;
}

// 10.3.2 [[Construct]]
Result<Object*> BuiltinFunction::Construct(Args args, Object* newTarget) {
  TRY(CheckStack());
  ExecutionContext context;
  context.realm = realm_;
  context.function = this;
  VM& v = vm();
  v.pushContext(&context);
  NativeCall call{Value::undefined(), args, newTarget ? newTarget : this, this};
  Result<Value> result = fn_(call);
  v.popContext();
  if (result.isThrow()) return Throw{result.thrown()};
  Value value = result.value();
  if (!value.isObject()) return ThrowTypeError("constructor did not return an object");
  return value.asObject();
}

void BuiltinFunction::trace(Tracer& tracer) {
  FunctionObject::trace(tracer);
  tracer.visit(initialName);
  tracer.visitAll(slots);
}

String* FunctionNameFromKey(const PropertyKey& name, std::u16string_view prefix) {
  std::u16string units;
  if (name.isSymbol()) {
    Value description = name.asSymbol()->description();
    if (!description.isUndefined()) {
      units = u"[";
      units.append(description.asString()->view());
      units.append(u"]");
    }
  } else {
    units = name.toValue().asString()->view();
  }
  if (!prefix.empty()) {
    std::u16string full(prefix);
    full.push_back(u' ');
    full.append(units);
    units = std::move(full);
  }
  return newString(std::move(units));
}

void SetFunctionName(Object* function, const PropertyKey& name, std::u16string_view prefix) {
  String* value = FunctionNameFromKey(name, prefix);
  (void)function->DefineOwnProperty(PropertyKey(u"name"), PropertyDescriptor::data(value, Attr::Configurable));
}

void SetFunctionLength(Object* function, double length) {
  (void)function->DefineOwnProperty(PropertyKey(u"length"),
                                    PropertyDescriptor::data(Value::number(length), Attr::Configurable));
}

}  // namespace snowman
