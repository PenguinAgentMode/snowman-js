// Unicode normalization (UAX #15) for String.prototype.normalize and localeCompare.
#pragma once

#include <string>
#include <string_view>

namespace snowman::builtins {

enum class NormalizationForm { NFC, NFD, NFKC, NFKD };

std::u16string Normalize(std::u16string_view input, NormalizationForm form);

}  // namespace snowman::builtins
