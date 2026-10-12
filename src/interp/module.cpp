// Modules (16.2). Work in progress: not supported yet.
#include "interp/module.h"

#include "runtime/errors.h"

namespace snowman::interp {

Environment* moduleEnvironmentOf(ModuleRecord*) { return nullptr; }

Result<void> runModuleFile(Realm*, const std::string&, std::u16string) {
  return ThrowSyntaxError("modules are not supported yet");
}

Result<Value> evaluateImportCall(const ast::ImportExpression*) {
  return ThrowSyntaxError("import() is not supported yet");
}

Result<Value> evaluateImportMeta() { return ThrowSyntaxError("import.meta is not supported yet"); }

}  // namespace snowman::interp
