// Bound function exotic objects (10.4.1).
#include "runtime/bound_function.h"

#include "runtime/operations.h"
#include "runtime/vm.h"

namespace snowman {

BoundFunction::BoundFunction(Object* prototype, Object* target, Value boundThis, std::vector<Value> boundArgs)
    : Object(prototype), boundTargetFunction(target), boundThis(boundThis), boundArguments(std::move(boundArgs)) {}

void BoundFunction::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(reinterpret_cast<Cell*>(boundTargetFunction));
  tracer.visit(boundThis);
  tracer.visitAll(boundArguments);
}

// 10.4.1.1 [[Call]]
Result<Value> BoundFunction::Call(Value, Args args) {
  TRY(CheckStack());
  ValueVector list;
  list.reserve(boundArguments.size() + args.size());
  for (Value v : boundArguments) list.push_back(v);
  for (Value v : args) list.push_back(v);
  return snowman::Call(boundTargetFunction, boundThis, list);
}

// 10.4.1.2 [[Construct]]
Result<Object*> BoundFunction::Construct(Args args, Object* newTarget) {
  TRY(CheckStack());
  ValueVector list;
  list.reserve(boundArguments.size() + args.size());
  for (Value v : boundArguments) list.push_back(v);
  for (Value v : args) list.push_back(v);
  if (newTarget == this) newTarget = boundTargetFunction;
  return snowman::Construct(boundTargetFunction, list, newTarget);
}

// 10.4.1.3 BoundFunctionCreate
Result<BoundFunction*> BoundFunctionCreate(Object* targetFunction, Value boundThis, Args boundArgs) {
  Object* proto = TRY(targetFunction->GetPrototypeOf());
  return make<BoundFunction>(proto, targetFunction, boundThis, std::vector<Value>(boundArgs.begin(), boundArgs.end()));
}

}  // namespace snowman
