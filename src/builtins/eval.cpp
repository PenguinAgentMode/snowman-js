// eval (19.2.1): the global function. Direct eval is the interpreter's (PerformEval with direct = true).
#include "interp/interpreter.h"
#include "runtime/builtins.h"

namespace snowman::interp {

namespace {

Result<Value> globalEval(NativeCall& call) { return PerformEval(call.args[0], false, false); }

void installEval(Realm& realm) {
  BuiltinFunction* eval = createBuiltinFunction(realm, globalEval, 1, PropertyKey(u"eval"));
  realm.setIntrinsic(Intrinsic::eval, eval);
  defineGlobal(realm, PropertyKey(u"eval"), Value(eval));
}

}  // namespace

SNOWMAN_BUILTIN(eval, kOrderDefault, installEval);

}  // namespace snowman::interp
