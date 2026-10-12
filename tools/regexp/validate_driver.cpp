// validate_driver: snowman::regexp::validate over a batch, for the node oracle (oracle.mjs).
// stdin, one case per line: <flags> TAB <pattern>, each as hex UTF-16 code units (4 digits per unit).
// stdout, one line per case: "1" (valid) or "0 <message>".
//   g++ -std=c++20 -O2 -Isrc -o build/validate_driver tools/regexp/validate_driver.cpp src/regexp/*.cpp src/unicode/*.cpp
#include <iostream>
#include <string>

#include "regexp/regexp.h"

static std::u16string unhex(const std::string& s) {
  std::u16string out;
  for (size_t i = 0; i + 4 <= s.size(); i += 4) out += static_cast<char16_t>(std::stoul(s.substr(i, 4), nullptr, 16));
  return out;
}

int main() {
  std::ios::sync_with_stdio(false);
  std::string line;
  while (std::getline(std::cin, line)) {
    size_t tab = line.find('\t');
    std::u16string flags = unhex(line.substr(0, tab));
    std::u16string pattern = tab == std::string::npos ? std::u16string() : unhex(line.substr(tab + 1));
    auto error = snowman::regexp::validate(pattern, flags);
    if (error) {
      std::cout << "0 " << *error << '\n';
    } else {
      std::cout << "1\n";
    }
  }
}
