// --parse-only and --dump-ast (src/engine.h). Owned by the parser workstream.
#include <iostream>

#include "engine.h"

namespace snowman {

int parseOnlyMain(const CliOptions&, const std::string&) {
  std::cout << "Error: parser not implemented\n";
  return 1;
}

int dumpAstMain(const CliOptions&, const std::string&) {
  std::cout << "Error: parser not implemented\n";
  return 1;
}

}  // namespace snowman
