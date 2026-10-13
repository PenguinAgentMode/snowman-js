// Error (20.5.1–20.5.3), the NativeError constructors (20.5.5–20.5.6) and AggregateError (20.5.7).
#include "builtins/util.h"
#include "runtime/function.h"

namespace snowman::builtins {

namespace {

// 20.5.8.1 InstallErrorCause ( O, options )
Result<void> InstallErrorCause(Object* o, Value options) {
  if (options.isObject()) {
    bool has = TRY(HasProperty(options.asObject(), PropertyKey(u"cause")));
    if (has) {
      Value cause = TRY(Get(options.asObject(), PropertyKey(u"cause")));
      TRY(CreateNonEnumerableDataPropertyOrThrow(o, PropertyKey(u"cause"), cause));
    }
  }
  return {};
}

// The shared body of Error ( message [ , options ] ) and every NativeError: the error's prototype
// intrinsic is in the callee's first slot.
Result<Value> errorConstructor(NativeCall& call, Intrinsic proto, Value message, Value options) {
  Object* newTarget = newTargetOrCallee(call);
  ErrorObject* o = TRY(OrdinaryCreateFromConstructor<ErrorObject>(newTarget, proto));
  if (!message.isUndefined()) {
    String* msg = TRY(ToString(message));
    TRY(CreateNonEnumerableDataPropertyOrThrow(o, PropertyKey(u"message"), msg));
  }
  TRY(InstallErrorCause(o, options));
  return Value(o);
}

Result<Value> Error_construct(NativeCall& call) {
  auto proto = Intrinsic(uint16_t(call.callee->slots[0].asNumber()));
  return errorConstructor(call, proto, call.args[0], call.args[1]);
}

// 20.5.7.1.1 AggregateError ( errors, message [ , options ] )
Result<Value> AggregateError_construct(NativeCall& call) {
  Object* newTarget = newTargetOrCallee(call);
  ErrorObject* o = TRY(OrdinaryCreateFromConstructor<ErrorObject>(newTarget, Intrinsic::AggregateErrorPrototype));
  if (!call.args[1].isUndefined()) {
    String* msg = TRY(ToString(call.args[1]));
    TRY(CreateNonEnumerableDataPropertyOrThrow(o, PropertyKey(u"message"), msg));
  }
  TRY(InstallErrorCause(o, call.args[2]));
  IteratorRecord record = TRY(GetIterator(call.args[0], IteratorKind::Sync));
  ValueVector errorsList = TRY(IteratorToList(record));
  TRY(DefinePropertyOrThrow(o, PropertyKey(u"errors"),
                            PropertyDescriptor::data(CreateArrayFromList(errorsList), Attr::WritableConfigurable)));
  return Value(o);
}

// SuppressedError ( error, suppressed, message )
Result<Value> SuppressedError_construct(NativeCall& call) {
  Object* newTarget = newTargetOrCallee(call);
  ErrorObject* o = TRY(OrdinaryCreateFromConstructor<ErrorObject>(newTarget, Intrinsic::SuppressedErrorPrototype));
  if (!call.args[2].isUndefined()) {
    String* msg = TRY(ToString(call.args[2]));
    TRY(CreateNonEnumerableDataPropertyOrThrow(o, PropertyKey(u"message"), msg));
  }
  TRY(CreateNonEnumerableDataPropertyOrThrow(o, PropertyKey(u"error"), call.args[0]));
  TRY(CreateNonEnumerableDataPropertyOrThrow(o, PropertyKey(u"suppressed"), call.args[1]));
  return Value(o);
}

// 20.5.2.1 Error.isError ( arg )
Result<Value> Error_isError(NativeCall& call) {
  return Value::boolean(call.args[0].isObject() && call.args[0].asObject()->is<ErrorObject>());
}

// 20.5.3.4 Error.prototype.toString ( )
Result<Value> ErrorPrototype_toString(NativeCall& call) {
  Value o = call.thisValue;
  if (!o.isObject()) return ThrowTypeError("Error.prototype.toString called on non-object");
  Value nameValue = TRY(Get(o.asObject(), PropertyKey(u"name")));
  String* name = nameValue.isUndefined() ? atom(u"Error") : TRY(ToString(nameValue));
  Value msgValue = TRY(Get(o.asObject(), PropertyKey(u"message")));
  String* msg = msgValue.isUndefined() ? emptyString() : TRY(ToString(msgValue));
  if (name->length() == 0) return Value(msg);
  if (msg->length() == 0) return Value(name);
  return Value(concatStrings({name->view(), u": ", msg->view()}));
}

BuiltinFunction* defineErrorType(Realm& realm, Intrinsic ctorId, Intrinsic protoId, const char16_t* name,
                                 Object* protoParent, Object* ctorParent) {
  Object* proto = OrdinaryObjectCreate(protoParent);
  BuiltinFunction* ctor = defineConstructor(realm, ctorId, Error_construct, 1, PropertyKey(name), proto, protoId);
  ctor->slots.push_back(Value::number(double(uint16_t(protoId))));
  if (ctorParent) ctor->setPrototypeDirect(ctorParent);
  defineValue(proto, PropertyKey(u"message"), Value(emptyString()));
  defineValue(proto, PropertyKey(u"name"), str(name));
  return ctor;
}

void installErrors(Realm& realm) {
  Object* objectProto = realm.intrinsic(Intrinsic::ObjectPrototype);
  BuiltinFunction* error =
      defineErrorType(realm, Intrinsic::Error, Intrinsic::ErrorPrototype, u"Error", objectProto, nullptr);
  Object* errorProto = realm.intrinsic(Intrinsic::ErrorPrototype);
  defineFunction(realm, errorProto, PropertyKey(u"toString"), 0, ErrorPrototype_toString);
  defineFunction(realm, error, PropertyKey(u"isError"), 1, Error_isError);

  defineErrorType(realm, Intrinsic::EvalError, Intrinsic::EvalErrorPrototype, u"EvalError", errorProto, error);
  defineErrorType(realm, Intrinsic::RangeError, Intrinsic::RangeErrorPrototype, u"RangeError", errorProto, error);
  defineErrorType(realm, Intrinsic::ReferenceError, Intrinsic::ReferenceErrorPrototype, u"ReferenceError", errorProto, error);
  defineErrorType(realm, Intrinsic::SyntaxError, Intrinsic::SyntaxErrorPrototype, u"SyntaxError", errorProto, error);
  defineErrorType(realm, Intrinsic::TypeError, Intrinsic::TypeErrorPrototype, u"TypeError", errorProto, error);
  defineErrorType(realm, Intrinsic::URIError, Intrinsic::URIErrorPrototype, u"URIError", errorProto, error);

  Object* aggregateProto = OrdinaryObjectCreate(errorProto);
  BuiltinFunction* aggregate = defineConstructor(realm, Intrinsic::AggregateError, AggregateError_construct, 2,
                                                 PropertyKey(u"AggregateError"), aggregateProto,
                                                 Intrinsic::AggregateErrorPrototype);
  aggregate->setPrototypeDirect(error);

  Object* suppressedProto = OrdinaryObjectCreate(errorProto);
  BuiltinFunction* suppressed = defineConstructor(realm, Intrinsic::SuppressedError, SuppressedError_construct, 3,
                                                  PropertyKey(u"SuppressedError"), suppressedProto,
                                                  Intrinsic::SuppressedErrorPrototype);
  suppressed->setPrototypeDirect(error);
  defineValue(suppressedProto, PropertyKey(u"message"), Value(emptyString()));
  defineValue(suppressedProto, PropertyKey(u"name"), str(u"SuppressedError"));
  defineValue(aggregateProto, PropertyKey(u"message"), Value(emptyString()));
  defineValue(aggregateProto, PropertyKey(u"name"), str(u"AggregateError"));
}

}  // namespace

SNOWMAN_BUILTIN(Error, kOrderFundamental, installErrors);

}  // namespace snowman::builtins
