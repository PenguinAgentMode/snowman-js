// The %Temporal% namespace object (proposal-temporal §1).
#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

Object* TemporalNamespace(Realm& realm) {
  if (Object* existing = realm.intrinsic(Intrinsic::Temporal)) return existing;
  Object* temporal = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  realm.setIntrinsic(Intrinsic::Temporal, temporal);
  defineValue(temporal, PropertyKey(builtins::symbols().toStringTag), builtins::str(u"Temporal"), Attr::Configurable);
  defineGlobal(realm, PropertyKey(u"Temporal"), Value(temporal));
  return temporal;
}

namespace {

void installTemporal(Realm& realm) { TemporalNamespace(realm); }

}  // namespace

SNOWMAN_BUILTIN(Temporal, kOrderDefault, installTemporal);

}  // namespace snowman::temporal
