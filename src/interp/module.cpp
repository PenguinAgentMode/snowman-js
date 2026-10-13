// Modules (16.2): Cyclic Module Records (Source Text Module Records and JSON modules), the host's module
// loading (specifiers resolve relative to the referrer's file, as test262's INTERPRETING.md says), linking,
// evaluation with top-level await, import() and import.meta. Loading is synchronous: HostLoadImportedModule
// calls FinishLoadingImportedModule before it returns.
#include "interp/module.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "interp/suspend.h"
#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/operations.h"

namespace snowman::interp {

void ModuleRecord::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(reinterpret_cast<Cell*>(realm));
  tracer.visit(environment);
  tracer.visit(namespaceObject);
  tracer.visit(importMeta);
  tracer.visit(jsonValue);
  tracer.visit(evaluationError);
  for (ModuleRecord* module : loadedModules) tracer.visit(module);
  tracer.visit(cycleRoot);
  tracer.visit(topLevelCapability);
  for (ModuleRecord* module : asyncParentModules) tracer.visit(module);
}

Environment* moduleEnvironmentOf(ModuleRecord* module) { return module->environment; }

namespace {

std::string mainPath;

// The order in which modules had [[AsyncEvaluation]] set to true.
uint64_t asyncEvaluationCounter = 0;

Realm* currentRealm() { return vm().currentRealm(); }

PromiseCapability* newPromiseCapability() {
  return NewPromiseCapability(Value(currentRealm()->intrinsic(Intrinsic::Promise))).release();
}

PromiseObject* promiseOf(PromiseCapability* capability) {
  return capability->promise.asObject()->as<PromiseObject>();
}

// A built-in closure with internal slots.
BuiltinFunction* closure(NativeFn fn, int length, std::initializer_list<Value> slots) {
  BuiltinFunction* function = createBuiltinFunction(*currentRealm(), fn, length, PropertyKey(emptyString()));
  for (Value slot : slots) function->slots.push_back(slot);
  return function;
}

// GetImportedModule(referrer, request) for a request of referrer's program.
ModuleRecord* GetImportedModule(ModuleRecord* referrer, uint32_t request) { return referrer->loadedModules[request]; }

const std::u16string kDefault = u"default";

// ---- Exports ----

// GetExportedNames(exportStarSet)
void GetExportedNames(ModuleRecord* module, std::vector<ModuleRecord*>& exportStarSet,
                      std::vector<std::u16string>& exportedNames) {
  if (std::find(exportStarSet.begin(), exportStarSet.end(), module) != exportStarSet.end()) return;
  exportStarSet.push_back(module);
  if (module->isJSON()) {
    exportedNames.push_back(kDefault);
    return;
  }
  const ast::Program* program = module->program;
  for (const auto& e : program->localExportEntries) exportedNames.push_back(*e.exportName);
  for (const auto& e : program->indirectExportEntries) exportedNames.push_back(*e.exportName);
  for (const auto& e : program->starExportEntries) {
    ModuleRecord* requestedModule = GetImportedModule(module, *e.moduleRequest);
    std::vector<std::u16string> starNames;
    GetExportedNames(requestedModule, exportStarSet, starNames);
    for (std::u16string& n : starNames) {
      if (n == kDefault) continue;
      if (std::find(exportedNames.begin(), exportedNames.end(), n) == exportedNames.end()) {
        exportedNames.push_back(std::move(n));
      }
    }
  }
}

struct ResolveSetEntry {
  ModuleRecord* module;
  std::u16string_view exportName;
};

ResolvedBinding resolved(ModuleRecord* module, String* bindingName) {
  return {ResolvedBinding::Kind::Resolved, module, bindingName};
}

// ResolveExport(exportName, resolveSet)
ResolvedBinding resolveExport(ModuleRecord* module, std::u16string_view exportName,
                              std::vector<ResolveSetEntry>& resolveSet) {
  if (module->isJSON()) return exportName == kDefault ? resolved(module, atom(kDefault)) : ResolvedBinding{};
  for (const ResolveSetEntry& r : resolveSet) {
    if (r.module == module && r.exportName == exportName) return {};  // a circular import request
  }
  resolveSet.push_back({module, exportName});
  const ast::Program* program = module->program;
  for (const auto& e : program->localExportEntries) {
    if (*e.exportName == exportName) return resolved(module, atom(*e.localName));
  }
  for (const auto& e : program->indirectExportEntries) {
    if (*e.exportName != exportName) continue;
    ModuleRecord* importedModule = GetImportedModule(module, *e.moduleRequest);
    if (e.importKind == ast::ExportEntry::ImportKind::All) return resolved(importedModule, nullptr);
    return resolveExport(importedModule, e.importName, resolveSet);
  }
  if (exportName == kDefault) return {};
  ResolvedBinding starResolution;
  for (const auto& e : program->starExportEntries) {
    ModuleRecord* importedModule = GetImportedModule(module, *e.moduleRequest);
    ResolvedBinding resolution = resolveExport(importedModule, exportName, resolveSet);
    if (resolution.kind == ResolvedBinding::Kind::Ambiguous) return resolution;
    if (resolution.kind == ResolvedBinding::Kind::Null) continue;
    if (starResolution.kind == ResolvedBinding::Kind::Null) {
      starResolution = resolution;
    } else if (resolution.module != starResolution.module || resolution.bindingName != starResolution.bindingName) {
      return {ResolvedBinding::Kind::Ambiguous, nullptr, nullptr};
    }
  }
  return starResolution;
}

ResolvedBinding resolveExport(ModuleRecord* module, std::u16string_view exportName) {
  std::vector<ResolveSetEntry> resolveSet;
  return resolveExport(module, exportName, resolveSet);
}

}  // namespace

ResolvedBinding ResolveExport(ModuleRecord* module, String* exportName) {
  return resolveExport(module, exportName->view());
}

// 16.2.1.11 GetModuleNamespace(module)
ModuleNamespace* GetModuleNamespace(ModuleRecord* module) {
  if (module->namespaceObject) return module->namespaceObject;
  std::vector<ModuleRecord*> exportStarSet;
  std::vector<std::u16string> exportedNames;
  GetExportedNames(module, exportStarSet, exportedNames);
  std::vector<String*> unambiguousNames;
  for (const std::u16string& name : exportedNames) {
    if (resolveExport(module, name).isResolved()) unambiguousNames.push_back(atom(name));
  }
  module->namespaceObject = snowman::make<ModuleNamespace>(module, std::move(unambiguousNames));
  return module->namespaceObject;
}

void setHostMainPath(const std::string& path) {
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(path, error);
  mainPath = (error ? std::filesystem::path(path) : absolute).lexically_normal().string();
}

// ---- Loading ----

namespace {

// GraphLoadingState Record. Loading is synchronous, so it lives on the stack of LoadRequestedModules.
struct GraphLoadingState {
  PromiseCapability* promiseCapability = nullptr;
  bool isLoading = true;
  size_t pendingModulesCount = 1;
  std::vector<ModuleRecord*> visited;
};

// HostGetSupportedImportAttributes is « "type" ».
bool AllImportAttributesSupported(const std::vector<ast::ImportAttributeEntry>& attributes) {
  for (const auto& attribute : attributes) {
    if (attribute.key != u"type") return false;
  }
  return true;
}

const std::u16string* typeAttribute(const std::vector<ast::ImportAttributeEntry>& attributes) {
  for (const auto& attribute : attributes) {
    if (attribute.key == u"type") return &attribute.value;
  }
  return nullptr;
}

const std::string& referrerPath(Cell* referrer) {
  if (auto* module = dynamic_cast<ModuleRecord*>(referrer)) return module->path;
  return mainPath;
}

Realm* referrerRealm(Cell* referrer) {
  if (auto* module = dynamic_cast<ModuleRecord*>(referrer)) return module->realm;
  if (auto* script = dynamic_cast<ScriptRecord*>(referrer)) return script->realm;
  return currentRealm();
}

// The specifier as a path relative to the referrer's directory.
std::string resolveSpecifier(const std::string& referrer, std::u16string_view specifier) {
  std::filesystem::path base = std::filesystem::path(referrer).parent_path();
  return (base / utf16ToUtf8(specifier)).lexically_normal().string();
}

bool readFile(const std::string& path, std::string& out) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) return false;
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::ostringstream contents;
  contents << file.rdbuf();
  out = contents.str();
  return true;
}

ModuleRecord* newModule(Realm* realm, std::string path, ast::Program* program) {
  auto* module = snowman::make<ModuleRecord>(realm, std::move(path));
  module->program = program;
  if (program) {
    module->hasTLA = program->hasTopLevelAwait;
    module->loadedModules.resize(program->requestedModules.size(), nullptr);
  }
  return module;
}

// ParseModule(sourceText, realm, hostDefined)
Result<ModuleRecord*> ParseModule(std::u16string source, Realm* realm, std::string path) {
  parser::ParseOptions options;
  options.goal = ast::SourceType::Module;
  parser::ParseResult parsed = parser::parseProgram(std::move(source), options);
  if (!parsed.ok()) return throwParseError(*parsed.error);
  return newModule(realm, std::move(path), parsed.program);
}

// ParseJSONModule(source) + CreateDefaultExportSyntheticModule(json)
Result<ModuleRecord*> ParseJSONModule(std::u16string source, Realm* realm, std::string path) {
  Value parse = TRY(Get(realm->intrinsic(Intrinsic::JSON), PropertyKey(u"parse")));
  Value json = TRY(Call(parse, Value::undefined(), {Value(newString(std::move(source)))}));
  ModuleRecord* module = newModule(realm, std::move(path), nullptr);
  module->jsonValue = json;
  return module;
}

// HostLoadImportedModule(referrer, moduleRequest, ...) up to FinishLoadingImportedModule: the module for the
// request, from the referrer realm's module map or newly parsed.
Result<ModuleRecord*> loadImportedModule(Cell* referrer, const std::u16string& specifier,
                                         const std::vector<ast::ImportAttributeEntry>& attributes) {
  Realm* realm = referrerRealm(referrer);
  std::string path = resolveSpecifier(referrerPath(referrer), specifier);
  const std::u16string* type = typeAttribute(attributes);
  if (type && *type != u"json") {
    return ThrowTypeError("unsupported module type '" + utf16ToUtf8(*type) + "'");
  }
  bool json = type != nullptr;
  std::string key = json ? "json:" + path : path;
  auto& moduleMap = realmState(realm)->moduleMap;
  for (auto& entry : moduleMap) {
    if (entry.first == key) return entry.second;
  }
  std::string bytes;
  if (!readFile(path, bytes)) return ThrowTypeError("Cannot find module '" + utf16ToUtf8(specifier) + "'");
  std::u16string source = utf8ToUtf16(bytes);
  ModuleRecord* module = json ? TRY(ParseJSONModule(std::move(source), realm, path))
                              : TRY(ParseModule(std::move(source), realm, path));
  moduleMap.emplace_back(std::move(key), module);
  return module;
}

void InnerModuleLoading(GraphLoadingState& state, ModuleRecord* module);

// ContinueModuleLoading(state, moduleCompletion)
void ContinueModuleLoading(GraphLoadingState& state, Result<ModuleRecord*> moduleCompletion) {
  if (!state.isLoading) return;
  if (moduleCompletion.isNormal()) {
    InnerModuleLoading(state, moduleCompletion.value());
    return;
  }
  state.isLoading = false;
  (void)rejectCapability(state.promiseCapability, moduleCompletion.thrown());
}

// InnerModuleLoading(state, module)
void InnerModuleLoading(GraphLoadingState& state, ModuleRecord* module) {
  bool visited = std::find(state.visited.begin(), state.visited.end(), module) != state.visited.end();
  if (module->status == ModuleRecord::Status::New && !visited) {
    state.visited.push_back(module);
    if (module->program) {
      const auto& requests = module->program->requestedModules;
      state.pendingModulesCount += requests.size();
      for (uint32_t i = 0; i < requests.size(); i++) {
        if (!AllImportAttributesSupported(requests[i].attributes)) {
          ContinueModuleLoading(state, ThrowSyntaxError("unsupported import attribute"));
        } else if (module->loadedModules[i]) {
          InnerModuleLoading(state, module->loadedModules[i]);
        } else {
          Result<ModuleRecord*> result = loadImportedModule(module, requests[i].specifier, requests[i].attributes);
          // FinishLoadingImportedModule: record the module in [[LoadedModules]].
          if (result.isNormal()) module->loadedModules[i] = result.value();
          ContinueModuleLoading(state, result);
        }
        if (!state.isLoading) return;
      }
    }
  }
  state.pendingModulesCount--;
  if (state.pendingModulesCount != 0) return;
  state.isLoading = false;
  for (ModuleRecord* loaded : state.visited) {
    if (loaded->status == ModuleRecord::Status::New) loaded->status = ModuleRecord::Status::Unlinked;
  }
  (void)resolveCapability(state.promiseCapability, Value::undefined());
}

// LoadRequestedModules(): a promise settled by the time it returns.
PromiseCapability* LoadRequestedModules(ModuleRecord* module) {
  GraphLoadingState state;
  state.promiseCapability = newPromiseCapability();
  InnerModuleLoading(state, module);
  return state.promiseCapability;
}

// ---- Linking ----

Throw unresolvable(const std::u16string& name, ResolvedBinding resolution) {
  std::string text = utf16ToUtf8(name);
  if (resolution.kind == ResolvedBinding::Kind::Ambiguous) {
    return ThrowSyntaxError("The requested module contains conflicting star exports for name '" + text + "'");
  }
  return ThrowSyntaxError("The requested module does not provide an export named '" + text + "'");
}

// InitializeEnvironment() (16.2.1.6.4); a JSON module's is the Synthetic Module Record's Link().
Result<void> InitializeEnvironment(ModuleRecord* module) {
  auto* env = snowman::make<ModuleEnvironment>(globalEnvironment(module->realm));
  module->environment = env;
  if (module->isJSON()) {
    env->createInitializedBinding(atom(kDefault), Value::undefined());
    return {};
  }
  const ast::Program* program = module->program;
  for (const auto& e : program->indirectExportEntries) {
    ResolvedBinding resolution = resolveExport(module, *e.exportName);
    if (!resolution.isResolved()) return unresolvable(*e.exportName, resolution);
  }
  for (const auto& in : program->importEntries) {
    ModuleRecord* importedModule = GetImportedModule(module, in.moduleRequest);
    String* localName = atom(in.localName);
    if (in.isNamespaceObject) {
      env->createInitializedBinding(localName, Value(GetModuleNamespace(importedModule)), false);
      continue;
    }
    ResolvedBinding resolution = resolveExport(importedModule, in.importName);
    if (!resolution.isResolved()) return unresolvable(in.importName, resolution);
    if (!resolution.bindingName) {
      env->createInitializedBinding(localName, Value(GetModuleNamespace(resolution.module)), false);
    } else {
      env->CreateImportBinding(localName, resolution.module, resolution.bindingName);
    }
  }

  InterpContext moduleContext;
  moduleContext.realm = module->realm;
  moduleContext.scriptOrModule = module;
  moduleContext.variableEnvironment = env;
  moduleContext.lexicalEnvironment = env;
  moduleContext.strict = true;
  ContextScope scope(&moduleContext);
  for (const std::u16string& name : program->varNames) {
    String* dn = atom(name);
    if (!env->hasBindingDirect(dn)) env->createInitializedBinding(dn, Value::undefined());
  }
  for (const auto& d : program->lexicallyScopedDeclarations) {
    for (const std::u16string& name : d.boundNames) {
      String* dn = atom(name);
      if (d.isConstant) {
        TRY(env->CreateImmutableBinding(dn, true));
      } else {
        TRY(env->CreateMutableBinding(dn, false));
      }
      if (d.node->type == ast::NodeType::FunctionDeclaration) {
        Value fo = TRY(InstantiateFunctionObject(d.node->as<ast::FunctionDeclaration>(), env, nullptr));
        TRY(env->InitializeBinding(dn, fo));
      }
    }
  }
  return {};
}

// InnerModuleLinking(module, stack, index)
Result<uint32_t> InnerModuleLinking(ModuleRecord* module, std::vector<ModuleRecord*>& stack, uint32_t index) {
  using Status = ModuleRecord::Status;
  if (module->status != Status::Unlinked) return index;
  module->status = Status::Linking;
  module->dfsIndex = index;
  module->dfsAncestorIndex = index;
  index++;
  stack.push_back(module);
  for (ModuleRecord* requiredModule : module->loadedModules) {
    index = TRY(InnerModuleLinking(requiredModule, stack, index));
    if (requiredModule->status == Status::Linking) {
      module->dfsAncestorIndex = std::min(module->dfsAncestorIndex, requiredModule->dfsAncestorIndex);
    }
  }
  TRY(InitializeEnvironment(module));
  if (module->dfsAncestorIndex == module->dfsIndex) {
    ModuleRecord* requiredModule;
    do {
      requiredModule = stack.back();
      stack.pop_back();
      requiredModule->status = Status::Linked;
    } while (requiredModule != module);
  }
  return index;
}

// Link()
Result<void> Link(ModuleRecord* module) {
  std::vector<ModuleRecord*> stack;
  Result<uint32_t> result = InnerModuleLinking(module, stack, 0);
  if (result.isThrow()) {
    for (ModuleRecord* m : stack) m->status = ModuleRecord::Status::Unlinked;
    module->status = ModuleRecord::Status::Unlinked;
    return Throw{result.thrown()};
  }
  return {};
}

// ---- Evaluation ----

using Status = ModuleRecord::Status;

void AsyncModuleExecutionRejected(ModuleRecord* module, Value error);

ModuleRecord* moduleInSlot(NativeCall& call, size_t slot) {
  return static_cast<ModuleRecord*>(call.callee->slots[slot].asObject());
}

PromiseCapability* capabilityInSlot(NativeCall& call, size_t slot) {
  return static_cast<PromiseCapability*>(call.callee->slots[slot].asObject());
}

// The body of a module with top-level await, on its async coroutine.
Result<Value> asyncModuleBody(SuspendableObject* self) {
  auto* module = static_cast<ModuleRecord*>(self->context.scriptOrModule);
  TRY(evaluateStatementList(module->program->body));
  return Value::undefined();
}

// ExecuteModule([capability]); a JSON module's is the Synthetic Module Record's evaluation steps.
Result<void> ExecuteModule(ModuleRecord* module, PromiseCapability* capability = nullptr) {
  if (module->isJSON()) return module->environment->SetMutableBinding(atom(kDefault), module->jsonValue, true);
  InterpContext moduleContext;
  moduleContext.realm = module->realm;
  moduleContext.scriptOrModule = module;
  moduleContext.variableEnvironment = module->environment;
  moduleContext.lexicalEnvironment = module->environment;
  moduleContext.strict = true;
  if (!module->hasTLA) {
    ContextScope scope(&moduleContext);
    TRY(evaluateStatementList(module->program->body));
    return {};
  }
  AsyncNativeBlockStart(capability, asyncModuleBody, Value::undefined(), moduleContext);
  return {};
}

void AsyncModuleExecutionFulfilled(ModuleRecord* module);

Result<Value> asyncModuleFulfilled(NativeCall& call) {
  AsyncModuleExecutionFulfilled(moduleInSlot(call, 0));
  return Value::undefined();
}

Result<Value> asyncModuleRejected(NativeCall& call) {
  AsyncModuleExecutionRejected(moduleInSlot(call, 0), call.args[0]);
  return Value::undefined();
}

// ExecuteAsyncModule(module)
void ExecuteAsyncModule(ModuleRecord* module) {
  PromiseCapability* capability = newPromiseCapability();
  BuiltinFunction* onFulfilled = closure(asyncModuleFulfilled, 0, {Value(module)});
  BuiltinFunction* onRejected = closure(asyncModuleRejected, 0, {Value(module)});
  PerformPromiseThen(promiseOf(capability), Value(onFulfilled), Value(onRejected));
  (void)ExecuteModule(module, capability);
}

// GatherAvailableAncestors(module, execList)
void GatherAvailableAncestors(ModuleRecord* module, std::vector<ModuleRecord*>& execList) {
  for (ModuleRecord* m : module->asyncParentModules) {
    if (std::find(execList.begin(), execList.end(), m) != execList.end()) continue;
    // A module whose synchronous evaluation threw has no [[CycleRoot]] but has the error itself.
    ModuleRecord* root = m->cycleRoot ? m->cycleRoot : m;
    if (root->hasEvaluationError) continue;
    m->pendingAsyncDependencies--;
    if (m->pendingAsyncDependencies == 0) {
      execList.push_back(m);
      if (!m->hasTLA) GatherAvailableAncestors(m, execList);
    }
  }
}

// AsyncModuleExecutionFulfilled(module)
void AsyncModuleExecutionFulfilled(ModuleRecord* module) {
  if (module->status == Status::Evaluated) return;  // it already has an evaluation error
  module->asyncEvaluation = false;
  module->status = Status::Evaluated;
  if (module->topLevelCapability) (void)resolveCapability(module->topLevelCapability, Value::undefined());
  std::vector<ModuleRecord*> execList;
  GatherAvailableAncestors(module, execList);
  std::stable_sort(execList.begin(), execList.end(), [](ModuleRecord* a, ModuleRecord* b) {
    return a->asyncEvaluationOrder < b->asyncEvaluationOrder;
  });
  for (ModuleRecord* m : execList) {
    if (m->status == Status::Evaluated) continue;
    if (m->hasTLA) {
      ExecuteAsyncModule(m);
      continue;
    }
    Result<void> result = ExecuteModule(m);
    if (result.isThrow()) {
      AsyncModuleExecutionRejected(m, result.thrown());
      continue;
    }
    m->asyncEvaluation = false;
    m->status = Status::Evaluated;
    if (m->topLevelCapability) (void)resolveCapability(m->topLevelCapability, Value::undefined());
  }
}

// AsyncModuleExecutionRejected(module, error)
void AsyncModuleExecutionRejected(ModuleRecord* module, Value error) {
  if (module->status == Status::Evaluated) return;
  module->hasEvaluationError = true;
  module->evaluationError = error;
  module->status = Status::Evaluated;
  module->asyncEvaluation = false;
  for (ModuleRecord* m : module->asyncParentModules) AsyncModuleExecutionRejected(m, error);
  if (module->topLevelCapability) (void)rejectCapability(module->topLevelCapability, error);
}

// InnerModuleEvaluation(module, stack, index)
Result<uint32_t> InnerModuleEvaluation(ModuleRecord* module, std::vector<ModuleRecord*>& stack, uint32_t index) {
  if (module->status == Status::EvaluatingAsync || module->status == Status::Evaluated) {
    if (module->hasEvaluationError) return Throw{module->evaluationError};
    return index;
  }
  if (module->status == Status::Evaluating) return index;
  module->status = Status::Evaluating;
  module->dfsIndex = index;
  module->dfsAncestorIndex = index;
  module->pendingAsyncDependencies = 0;
  index++;
  stack.push_back(module);
  for (ModuleRecord* requiredModule : module->loadedModules) {
    index = TRY(InnerModuleEvaluation(requiredModule, stack, index));
    if (requiredModule->status == Status::Evaluating) {
      module->dfsAncestorIndex = std::min(module->dfsAncestorIndex, requiredModule->dfsAncestorIndex);
    } else {
      requiredModule = requiredModule->cycleRoot;
      if (requiredModule->hasEvaluationError) return Throw{requiredModule->evaluationError};
    }
    if (requiredModule->asyncEvaluation) {
      module->pendingAsyncDependencies++;
      requiredModule->asyncParentModules.push_back(module);
    }
  }
  if (module->pendingAsyncDependencies > 0 || module->hasTLA) {
    module->asyncEvaluation = true;
    module->asyncEvaluationOrder = ++asyncEvaluationCounter;
    if (module->pendingAsyncDependencies == 0) ExecuteAsyncModule(module);
  } else {
    TRY(ExecuteModule(module));
  }
  if (module->dfsAncestorIndex == module->dfsIndex) {
    ModuleRecord* requiredModule;
    do {
      requiredModule = stack.back();
      stack.pop_back();
      requiredModule->status = requiredModule->asyncEvaluation ? Status::EvaluatingAsync : Status::Evaluated;
      requiredModule->cycleRoot = module;
    } while (requiredModule != module);
  }
  return index;
}

// Evaluate(): the module's top-level promise capability.
PromiseCapability* Evaluate(ModuleRecord* module) {
  if ((module->status == Status::EvaluatingAsync || module->status == Status::Evaluated) && module->cycleRoot) {
    module = module->cycleRoot;
  }
  if (module->topLevelCapability) return module->topLevelCapability;
  std::vector<ModuleRecord*> stack;
  PromiseCapability* capability = newPromiseCapability();
  module->topLevelCapability = capability;
  Result<uint32_t> result = InnerModuleEvaluation(module, stack, 0);
  if (result.isThrow()) {
    for (ModuleRecord* m : stack) {
      m->status = Status::Evaluated;
      m->hasEvaluationError = true;
      m->evaluationError = result.thrown();
    }
    (void)rejectCapability(capability, result.thrown());
  } else if (!module->asyncEvaluation) {
    (void)resolveCapability(capability, Value::undefined());
  }
  return capability;
}

// ---- import() ----

Result<Value> rejectDynamicImport(NativeCall& call) {
  (void)rejectCapability(capabilityInSlot(call, 0), call.args[0]);
  return Value::undefined();
}

Result<Value> fulfillDynamicImport(NativeCall& call) {
  (void)resolveCapability(capabilityInSlot(call, 0), Value(GetModuleNamespace(moduleInSlot(call, 1))));
  return Value::undefined();
}

Result<Value> linkAndEvaluateDynamicImport(NativeCall& call) {
  PromiseCapability* promiseCapability = capabilityInSlot(call, 0);
  ModuleRecord* module = moduleInSlot(call, 1);
  Result<void> link = Link(module);
  if (link.isThrow()) {
    (void)rejectCapability(promiseCapability, link.thrown());
    return Value::undefined();
  }
  PromiseCapability* evaluatePromise = Evaluate(module);
  BuiltinFunction* onFulfilled = closure(fulfillDynamicImport, 0, {Value(promiseCapability), Value(module)});
  BuiltinFunction* onRejected = closure(rejectDynamicImport, 1, {Value(promiseCapability)});
  PerformPromiseThen(promiseOf(evaluatePromise), Value(onFulfilled), Value(onRejected));
  return Value::undefined();
}

// ContinueDynamicImport(promiseCapability, moduleCompletion)
void ContinueDynamicImport(PromiseCapability* promiseCapability, Result<ModuleRecord*> moduleCompletion) {
  if (moduleCompletion.isThrow()) {
    (void)rejectCapability(promiseCapability, moduleCompletion.thrown());
    return;
  }
  ModuleRecord* module = moduleCompletion.value();
  PromiseCapability* loadPromise = LoadRequestedModules(module);
  BuiltinFunction* onRejected = closure(rejectDynamicImport, 1, {Value(promiseCapability)});
  BuiltinFunction* linkAndEvaluate =
      closure(linkAndEvaluateDynamicImport, 0, {Value(promiseCapability), Value(module)});
  PerformPromiseThen(promiseOf(loadPromise), Value(linkAndEvaluate), Value(onRejected));
}

// The import attributes of import()'s options (EvaluateImportCall steps 10–11).
Result<std::vector<ast::ImportAttributeEntry>> importCallAttributes(Value options) {
  std::vector<ast::ImportAttributeEntry> attributes;
  if (options.isUndefined()) return attributes;
  if (!options.isObject()) return ThrowTypeError("The second argument of import() must be an object");
  Value attributesObj = TRY(Get(options.asObject(), PropertyKey(u"with")));
  if (attributesObj.isUndefined()) return attributes;
  if (!attributesObj.isObject()) return ThrowTypeError("The 'with' option of import() must be an object");
  ValueVector entries = TRY(EnumerableOwnProperties(attributesObj.asObject(), EnumerableOwnKind::KeyValue));
  for (Value entry : entries) {
    Value key = TRY(Get(entry.asObject(), PropertyKey::index(0)));
    Value value = TRY(Get(entry.asObject(), PropertyKey::index(1)));
    if (!value.isString()) return ThrowTypeError("Import attribute values must be strings");
    attributes.push_back({std::u16string(key.asString()->view()), std::u16string(value.asString()->view())});
  }
  if (!AllImportAttributesSupported(attributes)) return ThrowTypeError("Unsupported import attribute");
  std::sort(attributes.begin(), attributes.end(),
            [](const ast::ImportAttributeEntry& a, const ast::ImportAttributeEntry& b) { return a.key < b.key; });
  return attributes;
}

}  // namespace

// 13.3.10.1 EvaluateImportCall(specifierExpression, optionsExpression)
Result<Value> evaluateImportCall(const ast::ImportExpression* node) {
  Cell* referrer = GetActiveScriptOrModule();  // nullptr: the current Realm Record
  Value specifier = TRY(evaluate(node->source));
  Value options = Value::undefined();
  if (node->options) options = TRY(evaluate(node->options));
  PromiseCapability* promiseCapability = newPromiseCapability();
  Result<String*> specifierString = ToString(specifier);
  if (specifierString.isThrow()) {
    (void)rejectCapability(promiseCapability, specifierString.thrown());
    return promiseCapability->promise;
  }
  Result<std::vector<ast::ImportAttributeEntry>> attributes = importCallAttributes(options);
  if (attributes.isThrow()) {
    (void)rejectCapability(promiseCapability, attributes.thrown());
    return promiseCapability->promise;
  }
  // HostLoadImportedModule(referrer, moduleRequest, empty, promiseCapability), which finishes at once.
  std::u16string name(specifierString.value()->view());
  ContinueDynamicImport(promiseCapability, loadImportedModule(referrer, name, attributes.value()));
  return promiseCapability->promise;
}

// 13.3.12.1 import.meta
Result<Value> evaluateImportMeta() {
  auto* module = dynamic_cast<ModuleRecord*>(GetActiveScriptOrModule());
  if (!module) return ThrowSyntaxError("import.meta is only valid in module code");
  if (!module->importMeta) module->importMeta = OrdinaryObjectCreate(nullptr);
  return Value(module->importMeta);
}

// ---- The CLI ----

namespace {

// A settled promise's rejection reason, if it was rejected.
bool rejected(PromiseCapability* capability, Value& reason) {
  PromiseObject* promise = promiseOf(capability);
  if (promise->state != PromiseObject::State::Rejected) return false;
  promise->isHandled = true;
  reason = promise->result;
  return true;
}

}  // namespace

Result<void> runModuleFile(Realm* realm, const std::string& path, std::u16string source) {
  setHostMainPath(path);
  ModuleRecord* module = TRY(ParseModule(std::move(source), realm, mainPath));
  realmState(realm)->moduleMap.emplace_back(mainPath, module);
  Value reason;
  if (rejected(LoadRequestedModules(module), reason)) return Throw{reason};
  TRY(Link(module));
  PromiseCapability* evaluation = Evaluate(module);
  TRY(vm().runJobs());
  if (rejected(evaluation, reason)) return Throw{reason};
  return {};
}

}  // namespace snowman::interp
