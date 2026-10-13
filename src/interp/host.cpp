// The host: print and $262 (test262's INTERPRETING.md), per realm.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "interp/agent.h"
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

// $262.agent.monotonicNow(): milliseconds on a clock that never goes back (one origin for every agent).
Result<Value> hostAgentMonotonicNow(NativeCall&) { return Value::number(MonotonicMs()); }

// $262.agent.getReport(): the next report from an agent, or null.
Result<Value> hostAgentGetReport(NativeCall&) {
  std::optional<std::u16string> report = PopReport();
  if (!report) return Value::null();
  return Value(newString(std::move(*report)));
}

// $262.agent.start(source): a new agent (thread) running source as a Script.
Result<Value> hostAgentStart(NativeCall& call) {
  String* source = TRY(ToString(call.args[0]));
  if (!StartAgent(std::u16string(source->view()))) return ThrowRangeError("$262.agent.start: cannot start an agent");
  return Value::undefined();
}

// $262.agent.broadcast(sab [, id]): every agent's receiveBroadcast callback gets the buffer; returns once all took it.
Result<Value> hostAgentBroadcast(NativeCall& call) {
  auto* buffer = call.args[0].isObject() ? call.args[0].asObject()->as<ArrayBufferObject>() : nullptr;
  if (!buffer || !buffer->isShared()) return ThrowTypeError("$262.agent.broadcast requires a SharedArrayBuffer");
  double id = call.args[1].isUndefined() ? 0 : TRY(ToNumber(call.args[1]));
  std::optional<size_t> maxByteLength;
  if (buffer->isResizable()) maxByteLength = buffer->maxByteLength();
  BroadcastToAgents(BroadcastMessage{buffer->sharedBlock(), maxByteLength, id});
  return Value::undefined();
}

// In an agent: $262.agent.receiveBroadcast(callback), report(value), leaving().
Result<Value> hostAgentReceiveBroadcast(NativeCall& call) {
  SetBroadcastCallback(call.args[0]);
  return Value::undefined();
}
Result<Value> hostAgentReport(NativeCall& call) {
  String* text = TRY(ToString(call.args[0]));
  PushReport(std::u16string(text->view()));
  return Value::undefined();
}
Result<Value> hostAgentLeaving(NativeCall&) {
  SetLeaving();
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
  Object* agent = OrdinaryObjectCreate(realm->intrinsic(Intrinsic::ObjectPrototype));
  defineFunction(r, agent, PropertyKey(u"sleep"), 1, hostAgentSleep);
  defineFunction(r, agent, PropertyKey(u"monotonicNow"), 0, hostAgentMonotonicNow);
  if (InAgentThread()) {
    defineFunction(r, agent, PropertyKey(u"receiveBroadcast"), 1, hostAgentReceiveBroadcast);
    defineFunction(r, agent, PropertyKey(u"report"), 1, hostAgentReport);
    defineFunction(r, agent, PropertyKey(u"leaving"), 0, hostAgentLeaving);
  } else {
    defineFunction(r, agent, PropertyKey(u"start"), 1, hostAgentStart);
    defineFunction(r, agent, PropertyKey(u"broadcast"), 2, hostAgentBroadcast);
    defineFunction(r, agent, PropertyKey(u"getReport"), 0, hostAgentGetReport);
  }
  defineValue(host, PropertyKey(u"agent"), Value(agent));
  realmState(realm)->host262 = host;
  defineGlobal(r, PropertyKey(u"$262"), Value(host));
}

}  // namespace snowman::interp
