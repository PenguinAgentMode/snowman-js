// RegExp early errors (ECMA-262 2025 22.2.1 and Annex B.1.2): the flags, then the pattern through the parser.
#include "regexp/ast.h"
#include "regexp/regexp.h"

namespace snowman::regexp {

std::optional<std::string> validate(std::u16string_view pattern, std::u16string_view flags) {
  auto parsed = parseFlags(flags);
  if (!parsed) return std::string("Invalid regular expression flags");
  std::string error;
  if (!parsePattern(pattern, *parsed, &error)) return error;
  return std::nullopt;
}

}  // namespace snowman::regexp
