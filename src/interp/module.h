// Modules (16.2): Cyclic / Source Text Module Records, synthetic JSON modules, loading, linking, evaluation
// with top-level await, namespace objects, import() and import.meta.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "interp/interpreter.h"
#include "interp/promise.h"

namespace snowman::interp {

class ModuleRecord final : public Cell {
 public:
  enum class Status : uint8_t { New, Unlinked, Linking, Linked, Evaluating, EvaluatingAsync, Evaluated };

  ModuleRecord(Realm* realm, std::string path) : realm(realm), path(std::move(path)) {}
  void trace(Tracer& tracer) override;

  bool isSynthetic() const { return code == nullptr; }

  Realm* realm;
  std::string path;                       // the resolved file path (host-defined)
  ModuleEnvironment* environment = nullptr;
  Object* namespaceObject = nullptr;
  ast::Program* code = nullptr;           // Source Text Module Records
  Value syntheticDefault;                 // JSON modules: the parsed value, exported as "default"
  Object* importMeta = nullptr;

  // Cyclic Module Record fields.
  Status status = Status::New;
  bool hasEvaluationError = false;
  Value evaluationError;
  uint32_t dfsIndex = 0;
  uint32_t dfsAncestorIndex = 0;
  ModuleRecord* cycleRoot = nullptr;
  bool hasTLA = false;
  bool asyncEvaluation = false;
  uint64_t asyncEvaluationOrder = 0;
  PromiseCapability* topLevelCapability = nullptr;
  std::vector<ModuleRecord*> asyncParentModules;
  uint32_t pendingAsyncDependencies = 0;
  // [[LoadedModules]], parallel to code->requestedModules.
  std::vector<ModuleRecord*> loadedModules;
};

// The CLI's --module: load the file as the root module of a graph, link and evaluate it, drain the job
// queue; a throw is an uncaught exception (parse errors are SyntaxErrors).
Result<void> runModuleFile(Realm* realm, const std::string& path, std::u16string source);

// The directory scripts resolve dynamic import() specifiers against (the main script's).
void setScriptDirectory(const std::string& directory);

}  // namespace snowman::interp
