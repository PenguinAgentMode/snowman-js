// --parse-only and --dump-ast (src/engine.h). Owned by the parser workstream.
//
// --parse-only --module also loads the module graph (specifiers relative to the importing file) and runs
// the link-time checks of InitializeEnvironment: every import and indirect export must resolve
// (ResolveExport, incl. star exports and ambiguity). JSON modules (`with { type: "json" }`) must be JSON.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "engine.h"
#include "parser/dump.h"
#include "parser/parser.h"

namespace snowman {

namespace {

using ast::ExportEntry;
using ast::Program;

struct SyntaxFailure {
  std::string message;
};

parser::ParseResult parseSource(bool module, const std::string& source) {
  parser::ParseOptions parseOptions;
  parseOptions.goal = module ? ast::SourceType::Module : ast::SourceType::Script;
  return parser::parseProgram(parser::utf8ToUtf16(source), parseOptions);
}

std::optional<std::string> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// JSON.parse's grammar (ECMA-404), over UTF-16.
class JsonValidator {
 public:
  explicit JsonValidator(const std::u16string& s) : s_(s) {}
  bool valid() {
    skip();
    if (!value()) return false;
    skip();
    return i_ == s_.size();
  }

 private:
  const std::u16string& s_;
  size_t i_ = 0;
  void skip() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++;
  }
  bool literal(const char* word) {
    for (size_t k = 0; word[k]; k++, i_++)
      if (i_ >= s_.size() || s_[i_] != static_cast<char16_t>(word[k])) return false;
    return true;
  }
  bool digits() {
    size_t start = i_;
    while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') i_++;
    return i_ > start;
  }
  bool number() {
    if (i_ < s_.size() && s_[i_] == '-') i_++;
    if (i_ < s_.size() && s_[i_] == '0') {
      i_++;
    } else if (!digits()) {
      return false;
    }
    if (i_ < s_.size() && s_[i_] == '.') {
      i_++;
      if (!digits()) return false;
    }
    if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
      i_++;
      if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) i_++;
      if (!digits()) return false;
    }
    return true;
  }
  bool string() {
    i_++;  // "
    while (i_ < s_.size()) {
      char16_t c = s_[i_++];
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c != '\\') continue;
      if (i_ >= s_.size()) return false;
      char16_t e = s_[i_++];
      if (e == 'u') {
        for (int k = 0; k < 4; k++, i_++) {
          if (i_ >= s_.size() || !std::isxdigit(static_cast<unsigned char>(s_[i_] < 0x80 ? s_[i_] : 'g'))) return false;
        }
      } else if (!(e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't')) {
        return false;
      }
    }
    return false;
  }
  bool value() {
    if (i_ >= s_.size()) return false;
    char16_t c = s_[i_];
    if (c == '{') {
      i_++;
      skip();
      if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
      for (;;) {
        skip();
        if (i_ >= s_.size() || s_[i_] != '"' || !string()) return false;
        skip();
        if (i_ >= s_.size() || s_[i_] != ':') return false;
        i_++;
        skip();
        if (!value()) return false;
        skip();
        if (i_ < s_.size() && s_[i_] == ',') {
          i_++;
          continue;
        }
        if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
        return false;
      }
    }
    if (c == '[') {
      i_++;
      skip();
      if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
      for (;;) {
        skip();
        if (!value()) return false;
        skip();
        if (i_ < s_.size() && s_[i_] == ',') {
          i_++;
          continue;
        }
        if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
        return false;
      }
    }
    if (c == '"') return string();
    if (c == 't') return literal("true");
    if (c == 'f') return literal("false");
    if (c == 'n') return literal("null");
    return number();
  }
};

struct LoadFailure {
  std::string message;
};

struct Module {
  std::string path;
  bool json = false;
  Program* program = nullptr;
  std::vector<Module*> requested;  // parallel to program->requestedModules
};

struct Resolution {
  enum class Kind { Null, Ambiguous, Found } kind = Kind::Null;
  Module* module = nullptr;
  std::u16string binding;
  bool isNamespace = false;
};

class ModuleGraph {
 public:
  Module* add(const std::string& path, bool json, const std::string& source) {
    auto module = std::make_unique<Module>();
    module->path = path;
    module->json = json;
    Module* m = module.get();
    modules_[key(path, json)] = std::move(module);
    if (json) {
      if (!JsonValidator(parser::utf8ToUtf16(source)).valid()) throw SyntaxFailure{"JSON module " + path + " is not valid JSON"};
      return m;
    }
    parser::ParseResult result = parseSource(true, source);
    if (!result.ok()) throw SyntaxFailure{result.error->message + " (" + path + ":" + std::to_string(result.error->line) + ")"};
    m->program = result.program;
    pending_.push_back(m);
    return m;
  }

  void loadAll() {
    while (!pending_.empty()) {
      Module* m = pending_.back();
      pending_.pop_back();
      // A source phase import names a module's [[ModuleSource]] (host-provided), not a module to link: not loaded.
      for (const ast::ModuleRequest& request : m->program->requestedModules)
        m->requested.push_back(request.phase == ast::ImportPhase::Source ? nullptr : load(m, request));
    }
  }

  void link() {
    for (auto& [k, module] : modules_) {
      Module* m = module.get();
      if (m->json) continue;
      for (const ast::ImportEntry& ie : m->program->importEntries) {
        if (ie.isNamespaceObject || ie.isSource) continue;
        Resolution r = resolveExport(m->requested[ie.moduleRequest], ie.importName);
        if (r.kind != Resolution::Kind::Found)
          throw SyntaxFailure{"The requested module does not provide an export named '" + parser::utf16ToUtf8(ie.importName) +
                              (r.kind == Resolution::Kind::Ambiguous ? "' (ambiguous)" : "'")};
      }
      for (const ExportEntry& e : m->program->indirectExportEntries) {
        if (e.importKind == ExportEntry::ImportKind::All || e.importKind == ExportEntry::ImportKind::Source) continue;
        Resolution r = resolveExport(m, *e.exportName);
        if (r.kind != Resolution::Kind::Found)
          throw SyntaxFailure{"Cannot resolve the indirect export '" + parser::utf16ToUtf8(*e.exportName) + "'"};
      }
    }
  }

 private:
  std::map<std::string, std::unique_ptr<Module>> modules_;
  std::vector<Module*> pending_;

  static std::string key(const std::string& path, bool json) { return path + (json ? "|json" : "|js"); }

  Module* load(Module* from, const ast::ModuleRequest& request) {
    bool json = false;
    for (const ast::ImportAttributeEntry& attr : request.attributes) {
      if (attr.key != u"type") throw SyntaxFailure{"Unsupported import attribute '" + parser::utf16ToUtf8(attr.key) + "'"};
      if (attr.value != u"json") throw SyntaxFailure{"Unsupported module type '" + parser::utf16ToUtf8(attr.value) + "'"};
      json = true;
    }
    std::filesystem::path base = std::filesystem::path(from->path).parent_path();
    std::string path = (base / parser::utf16ToUtf8(request.specifier)).lexically_normal().string();
    auto it = modules_.find(key(path, json));
    if (it != modules_.end()) return it->second.get();
    std::optional<std::string> source = readFile(path);
    if (!source) throw LoadFailure{"Cannot load module " + path};
    return add(path, json, *source);
  }

  Resolution resolveExport(Module* m, const std::u16string& exportName) {
    std::vector<std::pair<Module*, std::u16string>> resolveSet;
    return resolveExport(m, exportName, resolveSet);
  }

  // ResolveExport (16.2.1.6.3).
  Resolution resolveExport(Module* m, const std::u16string& exportName,
                           std::vector<std::pair<Module*, std::u16string>>& resolveSet) {
    Resolution found;
    found.kind = Resolution::Kind::Found;
    if (m->json) {
      if (exportName != u"default") return {};
      found.module = m;
      found.binding = u"default";
      return found;
    }
    for (auto& [module, name] : resolveSet)
      if (module == m && name == exportName) return {};
    resolveSet.emplace_back(m, exportName);
    const Program* p = m->program;
    for (const ExportEntry& e : p->localExportEntries) {
      if (e.exportName && *e.exportName == exportName) {
        found.module = m;
        found.binding = *e.localName;
        return found;
      }
    }
    for (const ExportEntry& e : p->indirectExportEntries) {
      if (!e.exportName || *e.exportName != exportName) continue;
      Module* imported = m->requested[*e.moduleRequest];
      if (e.importKind == ExportEntry::ImportKind::All || e.importKind == ExportEntry::ImportKind::Source) {
        found.module = imported;
        found.isNamespace = true;
        return found;
      }
      return resolveExport(imported, e.importName, resolveSet);
    }
    if (exportName == u"default") return {};
    Resolution star;
    for (const ExportEntry& e : p->starExportEntries) {
      Module* imported = m->requested[*e.moduleRequest];
      Resolution r = resolveExport(imported, exportName, resolveSet);
      if (r.kind == Resolution::Kind::Ambiguous) return r;
      if (r.kind == Resolution::Kind::Null) continue;
      if (star.kind == Resolution::Kind::Null) {
        star = r;
      } else if (r.module != star.module || r.isNamespace != star.isNamespace || r.binding != star.binding) {
        Resolution ambiguous;
        ambiguous.kind = Resolution::Kind::Ambiguous;
        return ambiguous;
      }
    }
    return star;
  }
};

void printSyntaxError(const parser::ParseError& error) {
  std::cout << "SyntaxError: " << error.message << " (" << error.line << ":" << error.column << ")\n";
}

}  // namespace

int parseOnlyMain(const CliOptions& options, const std::string& source) {
  if (!options.module) {
    parser::ParseResult result = parseSource(false, source);
    if (!result.ok()) {
      printSyntaxError(*result.error);
      return 1;
    }
    return 0;
  }
  try {
    ModuleGraph graph;
    std::string path = std::filesystem::absolute(options.file).lexically_normal().string();
    graph.add(path, false, source);
    graph.loadAll();
    graph.link();
  } catch (SyntaxFailure& failure) {
    std::cout << "SyntaxError: " << failure.message << "\n";
    return 1;
  } catch (LoadFailure& failure) {
    std::cout << "Error: " << failure.message << "\n";
    return 1;
  }
  return 0;
}

int dumpAstMain(const CliOptions& options, const std::string& source) {
  parser::ParseResult result = parseSource(options.module, source);
  if (!result.ok()) {
    printSyntaxError(*result.error);
    return 1;
  }
  std::cout << parser::dumpAstJson(result.program) << "\n";
  return 0;
}

}  // namespace snowman
