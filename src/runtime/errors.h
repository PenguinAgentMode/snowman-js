// Error objects and throwing them. INTERFACES.md section 6.
#pragma once

#include <string_view>

#include "runtime/object.h"
#include "runtime/realm.h"

namespace snowman {

// An object with an [[ErrorData]] slot.
class ErrorObject : public Object {
 public:
  explicit ErrorObject(Object* prototype) : Object(prototype) {}
};

// A new error in `realm` (prototype is e.g. Intrinsic::TypeErrorPrototype) with an own "message" property.
ErrorObject* CreateError(Realm* realm, Intrinsic prototype, std::string_view message);

// Create the error in the current realm and return it as a throw completion.
Throw ThrowError(Intrinsic prototype, std::string_view message);
Throw ThrowTypeError(std::string_view message);
Throw ThrowRangeError(std::string_view message);
Throw ThrowSyntaxError(std::string_view message);
Throw ThrowReferenceError(std::string_view message);
Throw ThrowEvalError(std::string_view message);
Throw ThrowURIError(std::string_view message);

}  // namespace snowman
