// Running scripts and modules (src/engine.h). Owned by the interp workstream.
#include <iostream>

#include "engine.h"

namespace snowman {

int runMain(const CliOptions&, const std::string&) {
  // Nothing runs yet: fail every test honestly rather than pass one by accident.
  std::cout << "Error: not implemented\n";
  return 1;
}

}  // namespace snowman
