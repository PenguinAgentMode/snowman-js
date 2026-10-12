// Creating and throwing error objects.
#include "runtime/errors.h"

#include "runtime/property_map.h"
#include "runtime/string.h"
#include "runtime/vm.h"

namespace snowman {

ErrorObject* CreateError(Realm* realm, Intrinsic prototype, std::string_view message) {
  Object* proto = realm ? realm->intrinsic(prototype) : nullptr;
  if (!proto && realm) proto = realm->intrinsic(Intrinsic::ErrorPrototype);
  if (!proto && realm) proto = realm->intrinsic(Intrinsic::ObjectPrototype);
  ErrorObject* error = make<ErrorObject>(proto);
  error->properties().put(PropertyKey(u"message"),
                          PropertySlot::data(newStringFromUtf8(message), Attr::WritableConfigurable));
  return error;
}

Throw ThrowError(Intrinsic prototype, std::string_view message) {
  return Throw{CreateError(vm().currentRealm(), prototype, message)};
}

Throw ThrowTypeError(std::string_view message) { return ThrowError(Intrinsic::TypeErrorPrototype, message); }
Throw ThrowRangeError(std::string_view message) { return ThrowError(Intrinsic::RangeErrorPrototype, message); }
Throw ThrowSyntaxError(std::string_view message) { return ThrowError(Intrinsic::SyntaxErrorPrototype, message); }
Throw ThrowReferenceError(std::string_view message) {
  return ThrowError(Intrinsic::ReferenceErrorPrototype, message);
}
Throw ThrowEvalError(std::string_view message) { return ThrowError(Intrinsic::EvalErrorPrototype, message); }
Throw ThrowURIError(std::string_view message) { return ThrowError(Intrinsic::URIErrorPrototype, message); }

}  // namespace snowman
