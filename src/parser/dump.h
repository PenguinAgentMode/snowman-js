// ESTree JSON for --dump-ast (acorn-compatible).
#pragma once

#include <string>

#include "parser/ast.h"

namespace snowman::parser {

std::string dumpAstJson(const ast::Program* program);

}  // namespace snowman::parser
