// The parser's API: source text in, an annotated ESTree AST (src/parser/ast.h) or a SyntaxError out.
// INTERFACES.md section 3. All early errors of ECMA-262 (incl. Annex B) are reported here, including the
// RegExp literal early errors (through snowman::regexp::validate).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "parser/ast.h"

namespace snowman::parser {

// Always a SyntaxError. line is 1-based, column 0-based (UTF-16 code units).
struct ParseError {
  std::string message;
  uint32_t line = 0, column = 0;
};

struct ParseOptions {
  ast::SourceType goal = ast::SourceType::Script;  // Module code is strict and allows top-level await
  // Script code parsed as eval code (PerformEval). Only changes what the fields below allow.
  bool eval = false;
  // The source starts in strict mode (a direct eval called from strict code).
  bool strict = false;
  // PerformEval's context for direct eval (all false for indirect eval and for scripts):
  bool inFunction = false;                // new.target allowed
  bool inMethod = false;                  // super.x allowed
  bool inDerivedConstructor = false;      // super(...) allowed
  bool inClassFieldInitializer = false;   // `arguments` forbidden (ContainsArguments)
  // Private names (without '#') of the classes enclosing the eval call: AllPrivateIdentifiersValid.
  std::vector<std::u16string> privateNames;
};

struct ParseResult {
  ast::Program* program = nullptr;                 // on success
  ast::FunctionExpression* function = nullptr;     // parseDynamicFunction: the function, inside program
  std::optional<ParseError> error;                 // on failure

  bool ok() const { return !error.has_value(); }
};

ParseResult parseProgram(std::u16string source, const ParseOptions& options = {});

enum class DynamicFunctionKind : uint8_t { Normal, Generator, Async, AsyncGenerator };

// CreateDynamicFunction (Function, GeneratorFunction, AsyncFunction, AsyncGeneratorFunction): `params` is
// the parameter strings already joined with ",", `body` the body string. Parses them separately (each must
// be a FormalParameters / FunctionBody on its own), then the synthesized source text
//   <prefix> anonymous(<params>\n) {\n<body>\n}
// which becomes Program::source and the function's sourceText. The Program's body holds a single
// ExpressionStatement with the FunctionExpression.
ParseResult parseDynamicFunction(DynamicFunctionKind kind, std::u16string params, std::u16string body);

// UTF-8 -> UTF-16 (ill-formed sequences become U+FFFD), and back (lone surrogates become U+FFFD).
std::u16string utf8ToUtf16(std::string_view utf8);
std::string utf16ToUtf8(std::u16string_view utf16);

}  // namespace snowman::parser
