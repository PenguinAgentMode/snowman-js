// --parse-only and --dump-ast (src/engine.h). Owned by the parser workstream.
#include <iostream>

#include "engine.h"
#include "parser/parser.h"

namespace snowman {

namespace {

parser::ParseResult parseFile(const CliOptions& options, const std::string& source) {
  parser::ParseOptions parseOptions;
  parseOptions.goal = options.module ? ast::SourceType::Module : ast::SourceType::Script;
  return parser::parseProgram(parser::utf8ToUtf16(source), parseOptions);
}

}  // namespace

int parseOnlyMain(const CliOptions& options, const std::string& source) {
  parser::ParseResult result = parseFile(options, source);
  if (!result.ok()) {
    std::cout << "SyntaxError: " << result.error->message << " (" << result.error->line << ":" << result.error->column
              << ")\n";
    return 1;
  }
  return 0;
}

int dumpAstMain(const CliOptions& options, const std::string& source) {
  parser::ParseResult result = parseFile(options, source);
  if (!result.ok()) {
    std::cout << "SyntaxError: " << result.error->message << "\n";
    return 1;
  }
  std::cout << "{}\n";
  return 0;
}

}  // namespace snowman
