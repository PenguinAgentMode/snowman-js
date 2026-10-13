// The host: print and $262 (test262's INTERPRETING.md), per realm.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

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

// $262.agent on the main agent (no other agents yet): sleep, monotonicNow, and getReport with nothing to report.
// $262.agent.sleep(ms): block the agent for ms milliseconds.
Result<Value> hostAgentSleep(NativeCall& call) {
  double ms = TRY(ToNumber(call.args[0]));
  if (std::isfinite(ms) && ms > 0) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
  return Value::undefined();
}

// $262.agent.monotonicNow(): milliseconds on a clock that never goes back.
Result<Value> hostAgentMonotonicNow(NativeCall&) {
  static const auto start = std::chrono::steady_clock::now();
  return Value::number(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

// $262.agent.getReport(): the next report from another agent, or null; there are none.
Result<Value> hostAgentGetReport(NativeCall&) { return Value::null(); }

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
  Object* agent = OrdinaryObjectCreate(realm->intrinsic(Intrinsic::ObjectPrototype));
  defineFunction(r, agent, PropertyKey(u"sleep"), 1, hostAgentSleep);
  defineFunction(r, agent, PropertyKey(u"monotonicNow"), 0, hostAgentMonotonicNow);
  defineFunction(r, agent, PropertyKey(u"getReport"), 0, hostAgentGetReport);
  defineValue(host, PropertyKey(u"agent"), Value(agent));
  realmState(realm)->host262 = host;
  defineGlobal(r, PropertyKey(u"$262"), Value(host));
}

}  // namespace snowman::interp
