// %AbstractModuleSource% (source phase imports): the abstract superclass of Module Source Objects.
#pragma once

#include "runtime/object.h"
#include "runtime/string.h"

namespace snowman::builtins {

// A Module Source Object of a host-defined module: its [[ModuleSourceClassName]] is what
// %AbstractModuleSource%.prototype[@@toStringTag] returns. Its prototype's prototype is %AbstractModuleSource.prototype%.
class ModuleSourceObject : public Object {
 public:
  ModuleSourceObject(Object* prototype, String* className) : Object(prototype), className(className) {}
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(className);
  }
  String* className;  // [[ModuleSourceClassName]]
};

}  // namespace snowman::builtins
