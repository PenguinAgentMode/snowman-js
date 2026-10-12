// The RegExp engine's public seam. Owned by the regexp workstream; signature changes go through the lead.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace snowman::regexp {

// Early errors for a RegularExpressionLiteral (and the RegExp constructor): nullopt when `flags` and
// `pattern` are valid per ECMA-262 22.2.1 (Annex B grammar without u/v, the u grammar, the v grammar),
// else a SyntaxError message. Strings are UTF-16 code units, as the lexer cooked them.
std::optional<std::string> validate(std::u16string_view pattern, std::u16string_view flags);

// The matcher API (compile/exec) is added here by the regexp workstream and documented in INTERFACES.md
// before the RegExp built-in consumes it.

}  // namespace snowman::regexp
