// ShadowRealm (proposal-shadowrealm): the constructor, ShadowRealm.prototype.evaluate / importValue, and wrapped
// function exotic objects, the callable boundary between two realms.
#include <cmath>

#include "builtins/util.h"
#include "interp/interpreter.h"
#include "interp/module.h"
#include "interp/promise.h"
#include "parser/parser.h"
#include "runtime/function.h"

namespace snowman::interp {

namespace {

using builtins::currentRealm;
using builtins::intrinsic;

// An object with a [[ShadowRealm]] slot.
class ShadowRealmObject final : public Object {
 public:
  explicit ShadowRealmObject(Object* prototype) : Object(prototype) {}
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(reinterpret_cast<Cell*>(shadowRealm));
  }
  Realm* shadowRealm = nullptr;  // [[ShadowRealm]]
};

Result<Value> GetWrappedValue(Realm* callerRealm, Value value);

// A wrapped function exotic object: [[WrappedTargetFunction]] and [[Realm]] (the realm it was created for).
class WrappedFunction final : public FunctionObject {
 public:
  WrappedFunction(Object* prototype, Realm* realm, Object* target) : FunctionObject(prototype, realm), target(target) {}

  // [[Call]] ( thisArgument, argumentsList ): PrepareForWrappedFunctionCall, then OrdinaryWrappedFunctionCall.
  Result<Value> Call(Value thisArgument, Args args) override {
    TRY(CheckStack());
    ExecutionContext calleeContext;
    calleeContext.realm = realm();
    calleeContext.function = this;
    ContextScope scope(&calleeContext);
    Realm* targetRealm = TRY(GetFunctionRealm(target));
    ValueVector wrappedArgs;
    for (Value arg : args) wrappedArgs.push_back(TRY(GetWrappedValue(targetRealm, arg)));
    Value wrappedThisArgument = TRY(GetWrappedValue(targetRealm, thisArgument));
    Result<Value> result = snowman::Call(Value(target), wrappedThisArgument, wrappedArgs);
    if (result.isThrow()) return ThrowTypeError("wrapped function threw across the ShadowRealm boundary");
    return GetWrappedValue(realm(), result.value());
  }

  void trace(Tracer& tracer) override {
    FunctionObject::trace(tracer);
    tracer.visit(target);
  }

  Object* target;  // [[WrappedTargetFunction]]
};

// CopyNameAndLength ( F, Target ), as Function.prototype.bind does it with no prefix and no bound arguments.
Result<void> CopyNameAndLength(Object* f, Object* target) {
  double l = 0;
  bool targetHasLength = TRY(HasOwnProperty(target, PropertyKey(u"length")));
  if (targetHasLength) {
    Value targetLen = TRY(Get(target, PropertyKey(u"length")));
    if (targetLen.isNumber()) {
      double tl = targetLen.asNumber();
      if (tl == INFINITY) {
        l = INFINITY;
      } else if (tl != -INFINITY) {
        l = std::max(TRY(ToIntegerOrInfinity(targetLen)), 0.0);
      }
    }
  }
  SetFunctionLength(f, l);
  Value targetName = TRY(Get(target, PropertyKey(u"name")));
  if (!targetName.isString()) targetName = Value(emptyString());
  SetFunctionName(f, PropertyKey(targetName.asString()), {});
  return {};
}

// WrappedFunctionCreate ( callerRealm, Target )
Result<Value> WrappedFunctionCreate(Realm* callerRealm, Object* target) {
  auto* wrapped = make<WrappedFunction>(callerRealm->intrinsic(Intrinsic::FunctionPrototype), callerRealm, target);
  if (CopyNameAndLength(wrapped, target).isThrow()) return ThrowTypeError("cannot copy the wrapped function's name and length");
  return Value(wrapped);
}

// GetWrappedValue ( callerRealm, value )
Result<Value> GetWrappedValue(Realm* callerRealm, Value value) {
  if (!value.isObject()) return value;
  if (!IsCallable(value)) return ThrowTypeError("a ShadowRealm boundary only passes primitives and callables");
  return WrappedFunctionCreate(callerRealm, value.asObject());
}

// ValidateShadowRealmObject ( O )
Result<ShadowRealmObject*> ValidateShadowRealmObject(Value o) {
  auto* realm = o.isObject() ? o.asObject()->as<ShadowRealmObject>() : nullptr;
  if (!realm) return ThrowTypeError("receiver is not a ShadowRealm");
  return realm;
}

// PerformShadowRealmEval ( sourceText, callerRealm, evalRealm ): parsed here first, so syntax errors are the
// caller realm's SyntaxErrors; the evaluation itself is an indirect eval running in evalRealm.
Result<Value> PerformShadowRealmEval(String* sourceText, Realm* callerRealm, Realm* evalRealm) {
  parser::ParseOptions options;
  options.eval = true;
  parser::ParseResult parsed = parser::parseProgram(std::u16string(sourceText->view()), options);
  if (!parsed.ok()) return throwParseError(*parsed.error);
  Result<Value> result = Value::undefined();
  {
    ExecutionContext evalContext;
    evalContext.realm = evalRealm;
    ContextScope scope(&evalContext);
    result = PerformEval(Value(sourceText), false, false);
  }
  if (result.isThrow()) return ThrowTypeError("ShadowRealm.prototype.evaluate: the evaluated code threw");
  return GetWrappedValue(callerRealm, result.value());
}

// ShadowRealm ( )
Result<Value> ShadowRealm_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor ShadowRealm requires 'new'");
  ShadowRealmObject* o =
      TRY(OrdinaryCreateFromConstructor<ShadowRealmObject>(call.newTarget, Intrinsic::ShadowRealmPrototype));
  Realm* realmRec = CreateRealm();
  setUpRealm(realmRec);
  o->shadowRealm = realmRec;
  return Value(o);
}

// ShadowRealm.prototype.evaluate ( sourceText )
Result<Value> ShadowRealmPrototype_evaluate(NativeCall& call) {
  ShadowRealmObject* o = TRY(ValidateShadowRealmObject(call.thisValue));
  if (!call.args[0].isString()) return ThrowTypeError("ShadowRealm.prototype.evaluate requires a string");
  return PerformShadowRealmEval(call.args[0].asString(), currentRealm(), o->shadowRealm);
}

// ExportGetter: slots [exportNameString]
Result<Value> ExportGetter(NativeCall& call) {
  Value exports = call.args[0];
  PropertyKey string(call.callee->slots[0].asString());
  bool hasOwn = TRY(HasOwnProperty(exports.asObject(), string));
  if (!hasOwn) return ThrowTypeError("ShadowRealm.prototype.importValue: the module has no such export");
  Value value = TRY(Get(exports.asObject(), string));
  return GetWrappedValue(call.callee->realm(), value);
}

Result<Value> ImportValueRejected(NativeCall&) {
  return ThrowTypeError("ShadowRealm.prototype.importValue: the module could not be imported");
}

// ShadowRealmImportValue ( specifierString, exportNameString, callerRealm, evalRealm )
Result<Value> ShadowRealmImportValue(String* specifierString, String* exportNameString, Realm* callerRealm,
                                     Realm* evalRealm) {
  PromiseCapability* innerCapability = TRY(NewPromiseCapability(Value(intrinsic(Intrinsic::Promise))));
  {
    ExecutionContext evalContext;
    evalContext.realm = evalRealm;
    ContextScope scope(&evalContext);
    ImportModuleForRealm(evalRealm, specifierString, innerCapability);
  }
  BuiltinFunction* onFulfilled = createBuiltinFunction(*callerRealm, ExportGetter, 1, PropertyKey(emptyString()));
  onFulfilled->slots.push_back(Value(exportNameString));
  BuiltinFunction* onRejected = createBuiltinFunction(*callerRealm, ImportValueRejected, 1, PropertyKey(emptyString()));
  PromiseCapability* promiseCapability = TRY(NewPromiseCapability(Value(intrinsic(Intrinsic::Promise))));
  return PerformPromiseThen(innerCapability->promise.asObject()->as<PromiseObject>(), Value(onFulfilled),
                            Value(onRejected), promiseCapability);
}

// ShadowRealm.prototype.importValue ( specifier, exportName )
Result<Value> ShadowRealmPrototype_importValue(NativeCall& call) {
  ShadowRealmObject* o = TRY(ValidateShadowRealmObject(call.thisValue));
  String* specifierString = TRY(ToString(call.args[0]));
  if (!call.args[1].isString()) return ThrowTypeError("ShadowRealm.prototype.importValue: exportName must be a string");
  return ShadowRealmImportValue(specifierString, call.args[1].asString(), currentRealm(), o->shadowRealm);
}

void installShadowRealm(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  defineConstructor(realm, Intrinsic::ShadowRealm, ShadowRealm_construct, 0, PropertyKey(u"ShadowRealm"), proto,
                    Intrinsic::ShadowRealmPrototype);
  defineFunction(realm, proto, PropertyKey(u"evaluate"), 1, ShadowRealmPrototype_evaluate);
  defineFunction(realm, proto, PropertyKey(u"importValue"), 2, ShadowRealmPrototype_importValue);
  defineValue(proto, PropertyKey(builtins::symbols().toStringTag), builtins::str(u"ShadowRealm"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(ShadowRealm, kOrderDefault, installShadowRealm);

}  // namespace snowman::interp
