// The seam between the CLI (src/main.cpp, lead-owned) and the workstreams that implement each mode.
// The CLI contract itself is in t262/README.md.
#pragma once

#include <string>
#include <vector>

namespace snowman {

struct CliOptions {
  bool module = false;
  bool parseOnly = false;
  bool dumpAst = false;
  std::string file;
  // Global Scripts run before the entry, and after it once it completed normally (INTERFACES.md section 1).
  std::vector<std::string> preludes;
  std::vector<std::string> epilogues;
  // --can-block=false: the main agent's [[CanBlock]] is false (test262's CanBlockIsFalse flag).
  bool canBlock = true;
};

// Each returns the process exit code. `source` is the file's bytes (UTF-8). Errors are printed to stdout
// as `Name: message`, exit code 1.

// --parse-only: parse with early errors (and, with --module, load and link the module graph); run nothing.
// Implemented by the parser workstream in src/parser/cli.cpp.
int parseOnlyMain(const CliOptions& options, const std::string& source);

// --dump-ast: print the program as ESTree JSON (acorn-compatible) on stdout. src/parser/cli.cpp.
int dumpAstMain(const CliOptions& options, const std::string& source);

// Default: run the preludes, the script or module, drain the job queue, then the epilogues. Implemented by the
// interp workstream in src/interp/cli.cpp.
int runMain(const CliOptions& options, const std::string& source);

}  // namespace snowman
