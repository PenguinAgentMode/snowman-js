// DisposeCapability records and DisposeResources (explicit resource management), shared by DisposableStack,
// AsyncDisposableStack and Iterator.prototype[@@dispose].
#pragma once

#include <vector>

#include "runtime/object.h"

namespace snowman::builtins {

enum class DisposeHint : uint8_t { Sync, Async };

// A DisposableResource Record.
struct DisposableResource {
  Value resourceValue;
  DisposeHint hint;
  Value disposeMethod;  // undefined or a callable
};

// A DisposeCapability Record, held by a cell so its values are traced.
class DisposeCapability final : public Object {
 public:
  DisposeCapability() : Object(nullptr) {}
  std::vector<DisposableResource> stack;
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    for (auto& r : stack) {
      tracer.visit(r.resourceValue);
      tracer.visit(r.disposeMethod);
    }
  }
};

// AddDisposableResource ( disposeCapability, V, hint [ , method ] ); method empty = not present.
Result<void> AddDisposableResource(DisposeCapability* capability, Value v, DisposeHint hint, Value method = Value::empty());
// A new SuppressedError { error, suppressed } in the current realm.
Object* NewSuppressedError(Value error, Value suppressed);

}  // namespace snowman::builtins
