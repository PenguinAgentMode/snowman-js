// Function objects: the [[Realm]]-carrying base, and built-in (native) functions. ECMAScript function
// objects derive from FunctionObject in src/interp/. INTERFACES.md sections 5 and 9.
#pragma once

#include <string_view>
#include <vector>

#include "runtime/object.h"

namespace snowman {

class Realm;
class BuiltinFunction;

// What a native function receives. newTarget is nullptr for [[Call]].
struct NativeCall {
  Value thisValue;
  Args args;
  Object* newTarget = nullptr;
  BuiltinFunction* callee = nullptr;
};
using NativeFn = Result<Value> (*)(NativeCall& call);

class FunctionObject : public Object {
 public:
  FunctionObject(Object* prototype, Realm* realm) : Object(prototype), realm_(realm) {}
  Realm* realm() const { return realm_; }
  bool isCallable() const override { return true; }
  void trace(Tracer& tracer) override;

  // The source text Function.prototype.toString returns for an ECMAScript function object (its
  // [[SourceText]]); nullptr for built-in functions, which print as NativeFunction syntax.
  virtual String* sourceText() const { return nullptr; }

 protected:
  Realm* realm_;
};

// A built-in function (10.3). [[Call]]/[[Construct]] push an execution context for realm() and call fn.
// For [[Construct]], fn sees call.newTarget != nullptr and must return an Object.
class BuiltinFunction : public FunctionObject {
 public:
  BuiltinFunction(Realm* realm, NativeFn fn, Object* prototype, bool isConstructor);

  bool isConstructor() const override { return constructor_; }
  Result<Value> Call(Value thisArgument, Args args) override;
  Result<Object*> Construct(Args args, Object* newTarget) override;

  NativeFn fn() const { return fn_; }
  // [[InitialName]] (for Function.prototype.toString), set by createBuiltinFunction.
  Value initialName;
  // Per-function internal slots (e.g. a promise's resolving functions' [[Promise]]). Traced.
  std::vector<Value> slots;

  void trace(Tracer& tracer) override;

 private:
  NativeFn fn_;
  bool constructor_;
};

// 10.2.9 SetFunctionName (prefix e.g. u"get"; Symbol names become "[description]") and 10.2.10
// SetFunctionLength. Both define the property with DefinePropertyOrThrow semantics on a fresh function.
void SetFunctionName(Object* function, const PropertyKey& name, std::u16string_view prefix = {});
void SetFunctionLength(Object* function, double length);
// The String SetFunctionName would use for `name` (with prefix), for [[InitialName]].
String* FunctionNameFromKey(const PropertyKey& name, std::u16string_view prefix = {});

}  // namespace snowman
