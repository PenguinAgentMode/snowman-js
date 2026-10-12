// %GeneratorPrototype% (27.5.1): next, return, throw.
#include "interp/suspend.h"
#include "runtime/builtins.h"
#include "runtime/symbol.h"

namespace snowman::interp {

namespace {

Result<Value> generatorNext(NativeCall& call) { return GeneratorResume(call.thisValue, call.args[0], Value::empty()); }

Result<Value> generatorReturn(NativeCall& call) {
  return GeneratorResumeAbrupt(call.thisValue, ResumeKind::Return, call.args[0], Value::empty());
}

Result<Value> generatorThrow(NativeCall& call) {
  return GeneratorResumeAbrupt(call.thisValue, ResumeKind::Throw, call.args[0], Value::empty());
}

void installGeneratorPrototype(Realm& realm) {
  Object* prototype = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::IteratorPrototype));
  realm.setIntrinsic(Intrinsic::GeneratorPrototype, prototype);
  defineFunction(realm, prototype, PropertyKey(u"next"), 1, generatorNext);
  defineFunction(realm, prototype, PropertyKey(u"return"), 1, generatorReturn);
  defineFunction(realm, prototype, PropertyKey(u"throw"), 1, generatorThrow);
  defineValue(prototype, PropertyKey(vm().symbols.toStringTag), Value(atom(u"Generator")), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(GeneratorPrototype, kOrderDefault, installGeneratorPrototype);

}  // namespace snowman::interp
