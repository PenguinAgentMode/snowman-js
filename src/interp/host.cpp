// The host: print and $262 (test262's INTERPRETING.md), per realm.
#include <cstdio>

#include "interp/interpreter.h"
#include "runtime/arraybuffer.h"
#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/heap.h"
#include "runtime/operations.h"
#include "runtime/wrappers.h"

namespace snowman::interp {

namespace {

// print(...args): ToString of each argument, joined by spaces, then a newline.
Result<Value> hostPrint(NativeCall& call) {
  std::string line;
  for (size_t i = 0; i < call.args.size(); i++) {
    if (i > 0) line += ' ';
    String* text = TRY(ToString(call.args[i]));
    line += text->toUtf8();
  }
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), stdout);
  return Value::undefined();
}

// $262.createRealm(): a new realm with its global environment and host bindings; returns its $262.
Result<Value> hostCreateRealm(NativeCall&) {
  Realm* realm = CreateRealm();
  setUpRealm(realm);
  return Value(realmState(realm)->host262);
}

// $262.evalScript(source): ParseScript + ScriptEvaluation in the current realm (the function's).
Result<Value> hostEvalScript(NativeCall& call) {
  String* source = TRY(ToString(call.args[0]));
  Realm* realm = vm().currentRealm();
  ScriptRecord* script = TRY(ParseScript(std::u16string(source->view()), realm));
  return ScriptEvaluation(script);
}

// $262.detachArrayBuffer(buffer, key): DetachArrayBuffer(buffer, key).
Result<Value> hostDetachArrayBuffer(NativeCall& call) {
  Value b = call.args[0];
  auto* buf = b.isObject() ? b.asObject()->as<ArrayBufferObject>() : nullptr;
  if (!buf) return ThrowTypeError("$262.detachArrayBuffer: not an ArrayBuffer");
  TRY(DetachArrayBuffer(buf, call.args[1]));
  return Value::null();
}

Result<Value> hostGc(NativeCall&) {
  heap().collect();
  return Value::undefined();
}

}  // namespace

void installHostBindings(Realm* realm) {
  ExecutionContext context;
  context.realm = realm;
  ContextScope scope(&context);
  Realm& r = *realm;
  defineFunction(r, realm->globalObject(), PropertyKey(u"print"), 0, hostPrint);
  Object* host = OrdinaryObjectCreate(realm->intrinsic(Intrinsic::ObjectPrototype));
  defineFunction(r, host, PropertyKey(u"createRealm"), 0, hostCreateRealm);
  defineFunction(r, host, PropertyKey(u"evalScript"), 1, hostEvalScript);
  defineFunction(r, host, PropertyKey(u"detachArrayBuffer"), 1, hostDetachArrayBuffer);
  defineFunction(r, host, PropertyKey(u"gc"), 0, hostGc);
  defineValue(host, PropertyKey(u"global"), Value(realm->globalObject()));
  defineValue(host, PropertyKey(u"AbstractModuleSource"), Value(realm->intrinsic(Intrinsic::AbstractModuleSource)));
  // $262.IsHTMLDDA (INTERPRETING.md): an [[IsHTMLDDA]] object; calling it returns null.
  defineValue(host, PropertyKey(u"IsHTMLDDA"),
              Value(make<HTMLDDAObject>(realm->intrinsic(Intrinsic::ObjectPrototype))));
  realmState(realm)->host262 = host;
  defineGlobal(r, PropertyKey(u"$262"), Value(host));
}

}  // namespace snowman::interp
