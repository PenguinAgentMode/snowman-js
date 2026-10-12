// The Proxy constructor (28.2).
#include "builtins/util.h"
#include "runtime/function.h"
#include "runtime/proxy.h"

namespace snowman::builtins {

namespace {

// 28.2.1.1 Proxy ( target, handler )
Result<Value> Proxy_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Proxy requires 'new'");
  return Value(TRY(ProxyCreate(call.args[0], call.args[1])));
}

// 28.2.2.1.1 Proxy Revocation Functions: slots[0] is [[RevocableProxy]] (null once revoked).
Result<Value> revoker(NativeCall& call) {
  Value p = call.callee->slots[0];
  if (p.isNull()) return Value::undefined();
  call.callee->slots[0] = Value::null();
  p.asObject()->as<ProxyObject>()->revoke();
  return Value::undefined();
}

// 28.2.2.1 Proxy.revocable ( target, handler )
Result<Value> Proxy_revocable(NativeCall& call) {
  ProxyObject* p = TRY(ProxyCreate(call.args[0], call.args[1]));
  BuiltinFunction* revoke = createBuiltinFunction(*currentRealm(), revoker, 0, PropertyKey(u""));
  revoke->slots.push_back(Value(p));
  Object* result = newPlainObject();
  TRY(CreateDataPropertyOrThrow(result, PropertyKey(u"proxy"), Value(p)));
  TRY(CreateDataPropertyOrThrow(result, PropertyKey(u"revoke"), Value(revoke)));
  return Value(result);
}

void installProxy(Realm& realm) {
  BuiltinFunction* ctor = createBuiltinFunction(realm, Proxy_construct, 2, PropertyKey(u"Proxy"), true);
  realm.setIntrinsic(Intrinsic::Proxy, ctor);
  defineGlobal(realm, PropertyKey(u"Proxy"), ctor);
  defineFunction(realm, ctor, PropertyKey(u"revocable"), 2, Proxy_revocable);
}

}  // namespace

SNOWMAN_BUILTIN(Proxy, kOrderDefault, installProxy);

}  // namespace snowman::builtins
