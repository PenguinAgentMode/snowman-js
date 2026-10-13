// snowman-js entry point. The CLI contract is in t262/README.md; each mode is implemented behind src/engine.h.
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "engine.h"

namespace {

std::optional<snowman::CliOptions> parseOptions(int argc, char** argv) {
  snowman::CliOptions options;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--module") {
      options.module = true;
      continue;
    }
    if (arg == "--parse-only") {
      options.parseOnly = true;
      continue;
    }
    if (arg == "--dump-ast") {
      options.dumpAst = true;
      continue;
    }
    if (arg == "--prelude" || arg == "--epilogue") {
      if (++i == argc) return std::nullopt;
      (arg == "--prelude" ? options.preludes : options.epilogues).push_back(argv[i]);
      continue;
    }
    if (arg.starts_with("--")) return std::nullopt;
    options.file = arg;
  }

  if (options.file.empty()) return std::nullopt;
  return options;
}

std::optional<std::string> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;

  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

}  // namespace

int main(int argc, char** argv) {
  auto options = parseOptions(argc, argv);
  if (!options) {
    std::cerr << "usage: engine [--prelude h.js]... [--epilogue e.js]... [--module] [--parse-only] [--dump-ast] file.js\n";
    return 64;
  }

  auto source = readFile(options->file);
  if (!source) {
    std::cout << "Error: cannot read " << options->file << "\n";
    return 1;
  }

  if (options->dumpAst) return snowman::dumpAstMain(*options, *source);
  if (options->parseOnly) return snowman::parseOnlyMain(*options, *source);
  return snowman::runMain(*options, *source);
}
