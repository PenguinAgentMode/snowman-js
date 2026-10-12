// The parser (src/parser/parser.h). Work in progress.
#include "parser/parser.h"

namespace snowman::parser {

ParseResult parseProgram(std::u16string, const ParseOptions&) {
  ParseResult result;
  result.error = ParseError{"parser not implemented", 1, 0};
  return result;
}

ParseResult parseDynamicFunction(DynamicFunctionKind, std::u16string, std::u16string) {
  ParseResult result;
  result.error = ParseError{"parser not implemented", 1, 0};
  return result;
}

}  // namespace snowman::parser
