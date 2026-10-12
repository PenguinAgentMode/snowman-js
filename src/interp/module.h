// Modules (16.2): Source Text Module Records, loading, linking, evaluation, namespace objects, import().
#pragma once

#include <string>

#include "interp/interpreter.h"

namespace snowman::interp {

// The CLI's --module: load the file as the root module of a graph, link and evaluate it, drain the job
// queue; a throw is an uncaught exception (parse errors are SyntaxErrors).
Result<void> runModuleFile(Realm* realm, const std::string& path, std::u16string source);

}  // namespace snowman::interp
