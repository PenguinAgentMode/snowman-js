// exec_driver: snowman::regexp::compile + exec over a batch, for exec-oracle.mjs.
// stdin, one case per line: <flags> TAB <pattern> TAB <input> TAB <lastIndex>, strings as hex UTF-16 code units
// (4 digits per unit). The caller's part of RegExpBuiltinExec is done here: lastIndex is 0 without g or y, and
// lastIndex > length fails.
// stdout, one line per case: "E <message>" (SyntaxError), "N" (no match), "X" (resource exhausted) or
// "M s0 e0 s1 e1 ..." (spans, -1 -1 for a group that did not participate).
//   g++ -std=c++20 -O2 -Isrc -o build/exec_driver tools/regexp/exec_driver.cpp src/regexp/*.cpp src/unicode/*.cpp
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "regexp/regexp.h"

static std::u16string unhex(const std::string& s) {
  std::u16string out;
  for (size_t i = 0; i + 4 <= s.size(); i += 4) out += static_cast<char16_t>(std::stoul(s.substr(i, 4), nullptr, 16));
  return out;
}

int main() {
  std::ios::sync_with_stdio(false);
  std::string line;
  std::vector<snowman::regexp::Span> spans;
  bool trace = std::getenv("EXEC_DRIVER_TRACE") != nullptr;  // case numbers to stderr, to find a slow one
  for (size_t n = 0; std::getline(std::cin, line); n++) {
    if (trace) std::cerr << n << std::endl;
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, '\t')) fields.push_back(field);
    while (fields.size() < 4) fields.emplace_back();
    std::string error;
    auto program = snowman::regexp::compile(unhex(fields[1]), unhex(fields[0]), &error);
    if (!program) {
      std::cout << "E " << error << '\n';
      continue;
    }
    std::u16string input = unhex(fields[2]);
    size_t lastIndex = fields[3].empty() ? 0 : std::stoul(fields[3]);
    const auto& flags = snowman::regexp::flagsOf(*program);
    if (!flags.global && !flags.sticky) lastIndex = 0;
    if (lastIndex > input.size()) {
      std::cout << "N\n";
      continue;
    }
    switch (snowman::regexp::exec(*program, input, lastIndex, &spans)) {
      case snowman::regexp::ExecStatus::NoMatch:
        std::cout << "N\n";
        break;
      case snowman::regexp::ExecStatus::ResourceExhausted:
        std::cout << "X\n";
        break;
      case snowman::regexp::ExecStatus::Match:
        std::cout << 'M';
        for (const auto& s : spans) std::cout << ' ' << s.start << ' ' << s.end;
        std::cout << '\n';
        break;
    }
  }
}
