// Boolean (20.3).
#include "builtins/util.h"
#include "runtime/wrappers.h"

namespace snowman::builtins {

namespace {

// 20.3.1.1 Boolean ( value )
Result<Value> Boolean_construct(NativeCall& call) {
  bool b = ToBoolean(call.args[0]);
  if (!call.newTarget) return Value::boolean(b);
  return Value(TRY(OrdinaryCreateFromConstructor<BooleanObject>(call.newTarget, Intrinsic::BooleanPrototype, b)));
}

// 20.3.3.3.1 ThisBooleanValue
Result<bool> thisBooleanValue(Value value) {
  if (value.isBoolean()) return value.asBoolean();
  if (value.isObject()) {
    if (auto* b = value.asObject()->as<BooleanObject>()) return b->booleanData;
  }
  return ThrowTypeError("Boolean.prototype method called on incompatible receiver");
}

Result<Value> BooleanPrototype_toString(NativeCall& call) {
  bool b = TRY(thisBooleanValue(call.thisValue));
  return str(b ? u"true" : u"false");
}

Result<Value> BooleanPrototype_valueOf(NativeCall& call) { return Value::boolean(TRY(thisBooleanValue(call.thisValue))); }

void installBoolean(Realm& realm) {
  Object* proto = make<BooleanObject>(realm.intrinsic(Intrinsic::ObjectPrototype), false);
  defineConstructor(realm, Intrinsic::Boolean, Boolean_construct, 1, PropertyKey(u"Boolean"), proto, Intrinsic::BooleanPrototype);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, BooleanPrototype_toString);
  defineFunction(realm, proto, PropertyKey(u"valueOf"), 0, BooleanPrototype_valueOf);
}

}  // namespace

SNOWMAN_BUILTIN(Boolean, kOrderDefault, installBoolean);

}  // namespace snowman::builtins
