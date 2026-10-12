// %AsyncGeneratorPrototype% (27.6.1): next, return, throw.
#include "interp/suspend.h"
#include "runtime/builtins.h"
#include "runtime/symbol.h"

namespace snowman::interp {

namespace {

Result<Value> asyncGeneratorNext(NativeCall& call) { return AsyncGeneratorNext(call.thisValue, call.args[0]); }
Result<Value> asyncGeneratorReturn(NativeCall& call) { return AsyncGeneratorReturn(call.thisValue, call.args[0]); }
Result<Value> asyncGeneratorThrow(NativeCall& call) { return AsyncGeneratorThrow(call.thisValue, call.args[0]); }

void installAsyncGeneratorPrototype(Realm& realm) {
  Object* prototype = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::AsyncIteratorPrototype));
  realm.setIntrinsic(Intrinsic::AsyncGeneratorPrototype, prototype);
  defineFunction(realm, prototype, PropertyKey(u"next"), 1, asyncGeneratorNext);
  defineFunction(realm, prototype, PropertyKey(u"return"), 1, asyncGeneratorReturn);
  defineFunction(realm, prototype, PropertyKey(u"throw"), 1, asyncGeneratorThrow);
  defineValue(prototype, PropertyKey(vm().symbols.toStringTag), Value(atom(u"AsyncGenerator")), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(AsyncGeneratorPrototype, kOrderDefault, installAsyncGeneratorPrototype);

}  // namespace snowman::interp
