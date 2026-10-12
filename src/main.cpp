// snowman-js entry point. The CLI contract is in t262/README.md.
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {

struct Options {
  bool module = false;
  bool parseOnly = false;
  std::string file;
};

std::optional<Options> parseOptions(int argc, char** argv) {
  Options options;
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
    std::cerr << "usage: engine [--module] [--parse-only] file.js\n";
    return 64;
  }

  auto source = readFile(options->file);
  if (!source) {
    std::cout << "Error: cannot read " << options->file << "\n";
    return 1;
  }

  // Nothing runs yet: fail every test honestly rather than pass one by accident.
  std::cout << "Error: not implemented\n";
  return 1;
}
