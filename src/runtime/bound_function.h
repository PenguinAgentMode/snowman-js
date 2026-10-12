// Bound function exotic objects (10.4.1).
#pragma once

#include "runtime/object.h"

namespace snowman {

class BoundFunction : public Object {
 public:
  BoundFunction(Object* prototype, Object* target, Value boundThis, std::vector<Value> boundArgs);

  bool isCallable() const override { return true; }
  bool isConstructor() const override { return boundTargetFunction->isConstructor(); }
  Result<Value> Call(Value thisArgument, Args args) override;
  Result<Object*> Construct(Args args, Object* newTarget) override;
  void trace(Tracer& tracer) override;

  Object* boundTargetFunction;
  Value boundThis;
  std::vector<Value> boundArguments;
};

// 10.4.1.3 BoundFunctionCreate
Result<BoundFunction*> BoundFunctionCreate(Object* targetFunction, Value boundThis, Args boundArgs);

}  // namespace snowman
