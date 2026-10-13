// Modules (16.2): Source Text Module Records and JSON modules as Cyclic Module Records, loading (the host's
// HostLoadImportedModule), linking, evaluation with top-level await, module namespace objects (10.4.6),
// import() and import.meta.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "interp/interpreter.h"
#include "interp/promise.h"

namespace snowman::interp {

class ModuleNamespace;

// A Cyclic Module Record (16.2.1.5). A Source Text Module Record has a program; a JSON module (a Synthetic
// Module Record whose only export is "default") has none and runs through the same algorithms as a module
// with no dependencies whose evaluation sets its "default" binding. An Object (never exposed) so built-in
// closures can hold it in their slots.
class ModuleRecord final : public Object {
 public:
  enum class Status : uint8_t { New, Unlinked, Linking, Linked, Evaluating, EvaluatingAsync, Evaluated };

  ModuleRecord(Realm* realm, std::string path) : Object(nullptr), realm(realm), path(std::move(path)) {}
  void trace(Tracer& tracer) override;

  bool isJSON() const { return program == nullptr; }

  // Module Record fields.
  Realm* realm;                                  // [[Realm]]
  ModuleEnvironment* environment = nullptr;      // [[Environment]], nullptr until linked
  ModuleNamespace* namespaceObject = nullptr;    // [[Namespace]]
  std::string path;                              // [[HostDefined]]: the resolved file path

  // Source Text Module Record fields ([[ImportEntries]] etc. are the program's).
  ast::Program* program = nullptr;               // [[ECMAScriptCode]]
  Object* importMeta = nullptr;                  // [[ImportMeta]]
  Value jsonValue;                               // a JSON module's default export

  // Cyclic Module Record fields.
  Status status = Status::New;
  bool hasEvaluationError = false;               // [[EvaluationError]] is a throw completion
  Value evaluationError;
  uint32_t dfsIndex = 0;
  uint32_t dfsAncestorIndex = 0;
  std::vector<ModuleRecord*> loadedModules;      // [[LoadedModules]], one per program->requestedModules entry
  ModuleRecord* cycleRoot = nullptr;
  bool hasTLA = false;
  bool asyncEvaluation = false;
  uint64_t asyncEvaluationOrder = 0;             // when [[AsyncEvaluation]] was set to true
  PromiseCapability* topLevelCapability = nullptr;
  std::vector<ModuleRecord*> asyncParentModules;
  uint32_t pendingAsyncDependencies = 0;
};

// A ResolvedBinding Record, null, or AMBIGUOUS. bindingName nullptr with a module is NAMESPACE.
struct ResolvedBinding {
  enum class Kind : uint8_t { Null, Ambiguous, Resolved };
  Kind kind = Kind::Null;
  ModuleRecord* module = nullptr;
  String* bindingName = nullptr;

  bool isResolved() const { return kind == Kind::Resolved; }
};

// ResolveExport(exportName) of a module (16.2.1.6.3).
ResolvedBinding ResolveExport(ModuleRecord* module, String* exportName);

// 10.4.6 Module Namespace Exotic Objects.
class ModuleNamespace final : public Object {
 public:
  ModuleNamespace(ModuleRecord* module, std::vector<String*> exports);
  void trace(Tracer& tracer) override;

  Result<Object*> GetPrototypeOf() override;
  Result<bool> SetPrototypeOf(Object* prototype) override;
  Result<bool> IsExtensible() override;
  Result<bool> PreventExtensions() override;
  Result<std::optional<PropertyDescriptor>> GetOwnProperty(const PropertyKey& key) override;
  Result<bool> DefineOwnProperty(const PropertyKey& key, const PropertyDescriptor& desc) override;
  Result<bool> HasProperty(const PropertyKey& key) override;
  Result<Value> Get(const PropertyKey& key, Value receiver) override;
  Result<bool> Set(const PropertyKey& key, Value value, Value receiver) override;
  Result<bool> Delete(const PropertyKey& key) override;
  Result<std::vector<PropertyKey>> OwnPropertyKeys() override;

  ModuleRecord* module;            // [[Module]]
  std::vector<String*> exports;    // [[Exports]], sorted by code units
  std::vector<PropertyKey> keys;   // exports as property keys

 private:
  // The index of key in exports, or -1.
  int find(const PropertyKey& key) const;
};

// GetModuleNamespace(module)
ModuleNamespace* GetModuleNamespace(ModuleRecord* module);

// The path of the main file (the CLI's), which scripts and realms resolve specifiers against.
void setHostMainPath(const std::string& path);

// The CLI's --module: load the file as the root module of a graph, link and evaluate it, drain the job
// queue; a throw is an uncaught exception (parse errors are SyntaxErrors).
Result<void> runModuleFile(Realm* realm, const std::string& path, std::u16string source);

}  // namespace snowman::interp
