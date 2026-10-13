// %AbstractModuleSource% (source phase imports, 28.3): an abstract class with no global binding ($262 exposes it).
#include "builtins/abstract_module_source.h"

#include "builtins/util.h"

namespace snowman::builtins {

namespace {

// %AbstractModuleSource% ( ): always throws.
Result<Value> AbstractModuleSource_construct(NativeCall&) {
  return ThrowTypeError("%AbstractModuleSource% is an abstract class and cannot be called or constructed");
}

// get %AbstractModuleSource%.prototype [ %Symbol.toStringTag% ]
Result<Value> AbstractModuleSourcePrototype_toStringTag(NativeCall& call) {
  if (!call.thisValue.isObject()) return Value::undefined();
  auto* source = call.thisValue.asObject()->as<ModuleSourceObject>();
  if (!source) return Value::undefined();
  return Value(source->className);
}

void installAbstractModuleSource(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  defineConstructor(realm, Intrinsic::AbstractModuleSource, AbstractModuleSource_construct, 0,
                    PropertyKey(u"AbstractModuleSource"), proto, Intrinsic::AbstractModuleSourcePrototype, false);
  defineAccessor(realm, proto, PropertyKey(symbols().toStringTag), AbstractModuleSourcePrototype_toStringTag, nullptr);
}

}  // namespace

SNOWMAN_BUILTIN(AbstractModuleSource, kOrderDefault, installAbstractModuleSource);

}  // namespace snowman::builtins
