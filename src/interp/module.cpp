// Modules (16.2): the host's loader (files resolved relative to the importing module), Source Text Module
// Records (ParseModule, ResolveExport, InitializeEnvironment, ExecuteModule), JSON modules, the Cyclic Module
// Record algorithms (Link, Evaluate with top-level await), module namespace objects, import() and import.meta.
#include "interp/module.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "builtins/abstract_module_source.h"
#include "interp/dispose.h"
#include "interp/suspend.h"
#include "runtime/arraybuffer.h"
#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/property_map.h"
#include "runtime/symbol.h"
#include "runtime/vm.h"

namespace snowman::interp {

void ModuleRecord::trace(Tracer& tracer) {
  tracer.visit(reinterpret_cast<Cell*>(realm));
  tracer.visit(environment);
  tracer.visit(namespaceObject);
  tracer.visit(deferredNamespaceObject);
  tracer.visit(moduleSource);
  tracer.visit(syntheticDefault);
  tracer.visit(importMeta);
  tracer.visit(evaluationError);
  tracer.visit(cycleRoot);
  tracer.visit(topLevelCapability);
  for (ModuleRecord* m : asyncParentModules) tracer.visit(m);
  for (ModuleRecord* m : loadedModules) tracer.visit(m);
}

Environment* moduleEnvironmentOf(ModuleRecord* module) { return module->environment; }

namespace {

std::string gScriptDirectory = ".";

std::string directoryOf(const std::string& path) {
  size_t slash = path.rfind('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

// The specifier resolved against a directory, with "." and ".." segments removed.
std::optional<std::string> resolveSpecifier(const std::string& directory, const std::string& specifier) {
  if (!specifier.starts_with("./") && !specifier.starts_with("../") && !specifier.starts_with("/")) {
    return std::nullopt;
  }
  std::string joined = specifier.starts_with("/") ? specifier : directory + "/" + specifier;
  bool absolute = joined.starts_with("/");
  std::vector<std::string> parts;
  std::stringstream stream(joined);
  std::string part;
  while (std::getline(stream, part, '/')) {
    if (part.empty() || part == ".") continue;
    if (part == ".." && !parts.empty() && parts.back() != "..") {
      parts.pop_back();
      continue;
    }
    parts.push_back(part);
  }
  std::string result = absolute ? "/" : "";
  for (size_t i = 0; i < parts.size(); i++) {
    if (i > 0) result += "/";
    result += parts[i];
  }
  return result;
}

std::optional<std::string> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// The module type a request's `type` import attribute names: JSON modules, text modules (import text), bytes
// modules (import bytes); anything else loads as JavaScript.
enum class ModuleType : uint8_t { JavaScript, Json, Text, Bytes };

ModuleType moduleTypeOf(const ast::ModuleRequest& request) {
  for (const auto& attribute : request.attributes) {
    if (attribute.key != u"type") continue;
    if (attribute.value == u"json") return ModuleType::Json;
    if (attribute.value == u"text") return ModuleType::Text;
    if (attribute.value == u"bytes") return ModuleType::Bytes;
  }
  return ModuleType::JavaScript;
}

// CreateBytesModule (import bytes): a Uint8Array over an immutable ArrayBuffer holding the file's bytes, as the
// default export.
Result<ModuleRecord*> createBytesModule(Realm* realm, const std::string& path, const std::string& bytes) {
  auto* module = snowman::make<ModuleRecord>(realm, path);
  ArrayBufferObject* buffer =
      TRY(AllocateImmutableArrayBuffer(realm->intrinsic(Intrinsic::ArrayBuffer), double(bytes.size()),
                                       reinterpret_cast<const uint8_t*>(bytes.data()), 0, bytes.size()));
  Object* uint8Array = TRY(Construct(realm->intrinsic(Intrinsic::Uint8Array), {Value(buffer)}));
  module->syntheticDefault = Value(uint8Array);
  module->status = ModuleRecord::Status::Unlinked;
  return module;
}

// ParseModule for a file's text, a JSON module (CreateDefaultExportSyntheticModule of ParseJSONModule) or a text
// module (CreateTextModule: the text as the default export).
Result<ModuleRecord*> createModule(Realm* realm, const std::string& path, std::u16string source, ModuleType type) {
  auto* module = snowman::make<ModuleRecord>(realm, path);
  if (type == ModuleType::Json) {
    Object* jsonObject = realm->intrinsic(Intrinsic::JSON);
    if (!jsonObject) return ThrowSyntaxError("JSON modules need the JSON built-in");
    Value parse = TRY(Get(jsonObject, PropertyKey(u"parse")));
    module->syntheticDefault = TRY(Call(parse, Value::undefined(), {Value(newString(std::move(source)))}));
    module->status = ModuleRecord::Status::Unlinked;
    return module;
  }
  if (type == ModuleType::Text) {
    module->syntheticDefault = Value(newString(std::move(source)));
    module->status = ModuleRecord::Status::Unlinked;
    return module;
  }
  parser::ParseOptions options;
  options.goal = ast::SourceType::Module;
  parser::ParseResult parsed = parser::parseProgram(std::move(source), options);
  if (!parsed.ok()) return throwParseError(*parsed.error);
  module->code = parsed.program;
  module->hasTLA = parsed.program->hasTopLevelAwait;
  module->loadedModules.assign(parsed.program->requestedModules.size(), nullptr);
  module->status = ModuleRecord::Status::Unlinked;
  return module;
}

// The host's `<module source>` module (INTERPRETING.md, source-phase-imports-module-source): a module with no
// exports whose [[ModuleSource]] is a ModuleSource object (prototype chain: ModuleSource.prototype, then
// %AbstractModuleSource.prototype%), one per realm.
ModuleRecord* createHostModuleSourceModule(Realm* realm) {
  auto* module = snowman::make<ModuleRecord>(realm, "<module source>");
  Object* prototype = OrdinaryObjectCreate(realm->intrinsic(Intrinsic::AbstractModuleSourcePrototype));
  module->moduleSource = snowman::make<builtins::ModuleSourceObject>(prototype, atom(u"ModuleSource"));
  module->syntheticDefault = Value(module->moduleSource);
  module->status = ModuleRecord::Status::Unlinked;
  return module;
}

// HostLoadImportedModule + FinishLoadingImportedModule: one module per resolved path (and type) per realm, the
// same for every import phase.
Result<ModuleRecord*> hostLoadImportedModule(Realm* realm, const std::string& directory,
                                             const ast::ModuleRequest& request) {
  std::string specifier = utf16ToUtf8(request.specifier);
  if (specifier == "<module source>") {
    RealmState* state = realmState(realm);
    for (auto& [loadedPath, module] : state->moduleMap) {
      if (loadedPath == specifier) return module;
    }
    ModuleRecord* module = createHostModuleSourceModule(realm);
    state->moduleMap.emplace_back(specifier, module);
    return module;
  }
  std::optional<std::string> path = resolveSpecifier(directory, specifier);
  if (!path) return ThrowTypeError("Cannot resolve module specifier '" + specifier + "'");
  ModuleType type = moduleTypeOf(request);
  std::string key = type == ModuleType::Json    ? *path + "#json"
                    : type == ModuleType::Text  ? *path + "#text"
                    : type == ModuleType::Bytes ? *path + "#bytes"
                                                : *path;
  RealmState* state = realmState(realm);
  for (auto& [loadedPath, module] : state->moduleMap) {
    if (loadedPath == key) return module;
  }
  std::optional<std::string> text = readFile(*path);
  if (!text) return ThrowTypeError("Cannot find module '" + *path + "'");
  ModuleRecord* module = type == ModuleType::Bytes ? TRY(createBytesModule(realm, *path, *text))
                                                   : TRY(createModule(realm, *path, utf8ToUtf16(*text), type));
  state->moduleMap.emplace_back(key, module);
  return module;
}

// LoadRequestedModules, synchronously: every module of the graph is loaded (or the first error thrown).
Result<void> loadRequestedModules(ModuleRecord* root) {
  std::vector<ModuleRecord*> visited;
  std::vector<ModuleRecord*> pending{root};
  while (!pending.empty()) {
    ModuleRecord* module = pending.back();
    pending.pop_back();
    if (module->isSynthetic() || std::find(visited.begin(), visited.end(), module) != visited.end()) continue;
    visited.push_back(module);
    const auto& requests = module->code->requestedModules;
    for (size_t i = 0; i < requests.size(); i++) {
      if (!module->loadedModules[i]) {
        module->loadedModules[i] = TRY(hostLoadImportedModule(module->realm, directoryOf(module->path), requests[i]));
      }
      // InnerModuleLoading: a source phase request loads that module alone (~single~), not its dependencies.
      if (requests[i].phase != ast::ImportPhase::Source) pending.push_back(module->loadedModules[i]);
    }
  }
  return {};
}

ModuleRecord* GetImportedModule(ModuleRecord* module, uint32_t request) { return module->loadedModules[request]; }

// ResolvedBinding Record. [[BindingName]] is a String (bindingName), or, with bindingName nullptr, ~namespace~,
// ~deferred-namespace~ (import defer) or ~source~ (source phase imports) as `special` says.
struct ResolvedBinding {
  enum class Kind { Null, Ambiguous, Binding } kind = Kind::Null;
  ModuleRecord* module = nullptr;
  String* bindingName = nullptr;
  enum class Special : uint8_t { None, Namespace, DeferredNamespace, Source } special = Special::None;
};

ast::ImportPhase requestPhase(const ModuleRecord* module, uint32_t request) {
  return module->code->requestedModules[request].phase;
}

struct ResolveSetEntry {
  ModuleRecord* module;
  std::u16string exportName;
};

// 16.2.1.6.3 ResolveExport(exportName, resolveSet)
ResolvedBinding ResolveExport(ModuleRecord* module, const std::u16string& exportName,
                              std::vector<ResolveSetEntry>& resolveSet) {
  if (module->isSynthetic()) {
    if (exportName == u"default") return {ResolvedBinding::Kind::Binding, module, atom(u"default")};
    return {};
  }
  for (const auto& r : resolveSet) {
    if (r.module == module && r.exportName == exportName) return {};
  }
  resolveSet.push_back({module, exportName});
  const ast::Program* code = module->code;
  for (const auto& e : code->localExportEntries) {
    if (e.exportName && *e.exportName == exportName) {
      return {ResolvedBinding::Kind::Binding, module, atom(*e.localName)};
    }
  }
  for (const auto& e : code->indirectExportEntries) {
    if (!e.exportName || *e.exportName != exportName) continue;
    ModuleRecord* importedModule = GetImportedModule(module, *e.moduleRequest);
    if (e.importKind == ast::ExportEntry::ImportKind::All) {
      bool deferred = requestPhase(module, *e.moduleRequest) == ast::ImportPhase::Defer;
      return {ResolvedBinding::Kind::Binding, importedModule, nullptr,
              deferred ? ResolvedBinding::Special::DeferredNamespace : ResolvedBinding::Special::Namespace};
    }
    if (e.importKind == ast::ExportEntry::ImportKind::Source) {
      return {ResolvedBinding::Kind::Binding, importedModule, nullptr, ResolvedBinding::Special::Source};
    }
    return ResolveExport(importedModule, e.importName, resolveSet);
  }
  if (exportName == u"default") return {};
  ResolvedBinding starResolution;
  for (const auto& e : code->starExportEntries) {
    ModuleRecord* importedModule = GetImportedModule(module, *e.moduleRequest);
    ResolvedBinding resolution = ResolveExport(importedModule, exportName, resolveSet);
    if (resolution.kind == ResolvedBinding::Kind::Ambiguous) return resolution;
    if (resolution.kind == ResolvedBinding::Kind::Null) continue;
    if (starResolution.kind == ResolvedBinding::Kind::Null) {
      starResolution = resolution;
    } else if (resolution.module != starResolution.module || resolution.bindingName != starResolution.bindingName ||
               resolution.special != starResolution.special) {
      return {ResolvedBinding::Kind::Ambiguous, nullptr, nullptr};
    }
  }
  return starResolution;
}

ResolvedBinding ResolveExport(ModuleRecord* module, const std::u16string& exportName) {
  std::vector<ResolveSetEntry> resolveSet;
  return ResolveExport(module, exportName, resolveSet);
}

// 16.2.1.6.2 GetExportedNames(exportStarSet)
std::vector<std::u16string> GetExportedNames(ModuleRecord* module, std::vector<ModuleRecord*>& exportStarSet) {
  if (module->isSynthetic()) return {u"default"};
  if (std::find(exportStarSet.begin(), exportStarSet.end(), module) != exportStarSet.end()) return {};
  exportStarSet.push_back(module);
  std::vector<std::u16string> exportedNames;
  const ast::Program* code = module->code;
  for (const auto& e : code->localExportEntries) exportedNames.push_back(*e.exportName);
  for (const auto& e : code->indirectExportEntries) exportedNames.push_back(*e.exportName);
  for (const auto& e : code->starExportEntries) {
    ModuleRecord* requestedModule = GetImportedModule(module, *e.moduleRequest);
    for (const auto& n : GetExportedNames(requestedModule, exportStarSet)) {
      if (n == u"default") continue;
      if (std::find(exportedNames.begin(), exportedNames.end(), n) == exportedNames.end()) exportedNames.push_back(n);
    }
  }
  return exportedNames;
}

enum class NamespacePhase : uint8_t { Evaluation, Defer };
Object* GetModuleNamespace(ModuleRecord* module, NamespacePhase phase = NamespacePhase::Evaluation);
Result<void> EvaluateModuleSync(ModuleRecord* module);

// The value of a ResolvedBinding Record whose [[BindingName]] is not a String: a namespace, a deferred
// namespace, or the module's [[ModuleSource]] (null when it has none).
Object* specialBindingValue(const ResolvedBinding& binding) {
  switch (binding.special) {
    case ResolvedBinding::Special::DeferredNamespace: return GetModuleNamespace(binding.module, NamespacePhase::Defer);
    case ResolvedBinding::Special::Source: return binding.module->moduleSource;
    default: return GetModuleNamespace(binding.module);
  }
}

// 10.4.6 Module namespace exotic objects, with [[Deferred]] (import defer): a deferred namespace evaluates its module
// (EvaluateModuleSync) before any access to its exports, i.e. in GetModuleExportsList.
class ModuleNamespace final : public Object {
 public:
  ModuleNamespace(ModuleRecord* module, std::vector<String*> exports, bool deferred)
      : Object(nullptr), module_(module), exports_(std::move(exports)), deferred_(deferred) {
    extensible_ = false;
  }

  Result<Object*> GetPrototypeOf() override { return static_cast<Object*>(nullptr); }
  Result<bool> SetPrototypeOf(Object* prototype) override { return prototype == nullptr; }
  Result<bool> IsExtensible() override { return false; }
  Result<bool> PreventExtensions() override { return true; }

  Result<std::optional<PropertyDescriptor>> GetOwnProperty(const PropertyKey& key) override {
    if (IsSymbolLikeNamespaceKey(key)) return Object::GetOwnProperty(key);
    TRY(GetModuleExportsList());
    if (!isExport(key)) return std::optional<PropertyDescriptor>();
    Value value = TRY(Get(key, Value(this)));
    return std::optional<PropertyDescriptor>(PropertyDescriptor::data(value, Attr::Writable | Attr::Enumerable));
  }

  Result<bool> DefineOwnProperty(const PropertyKey& key, const PropertyDescriptor& desc) override {
    if (IsSymbolLikeNamespaceKey(key)) return Object::DefineOwnProperty(key, desc);
    auto current = TRY(GetOwnProperty(key));
    if (!current) return false;
    if (desc.configurable && *desc.configurable) return false;
    if (desc.enumerable && !*desc.enumerable) return false;
    if (desc.isAccessorDescriptor()) return false;
    if (desc.writable && !*desc.writable) return false;
    if (desc.value) return SameValue(*desc.value, *current->value);
    return true;
  }

  Result<bool> HasProperty(const PropertyKey& key) override {
    if (IsSymbolLikeNamespaceKey(key)) return Object::HasProperty(key);
    TRY(GetModuleExportsList());
    return isExport(key);
  }

  Result<Value> Get(const PropertyKey& key, Value receiver) override {
    if (IsSymbolLikeNamespaceKey(key)) return Object::Get(key, receiver);
    TRY(GetModuleExportsList());
    if (!isExport(key)) return Value::undefined();
    ResolvedBinding binding = ResolveExport(module_, std::u16string(key.toValue().asString()->view()));
    ModuleRecord* targetModule = binding.module;
    if (!binding.bindingName) return Value(specialBindingValue(binding));
    if (targetModule->isSynthetic()) return targetModule->syntheticDefault;
    Environment* targetEnv = targetModule->environment;
    if (!targetEnv) return ThrowReferenceError("Cannot access a binding of a module that is not linked");
    return targetEnv->GetBindingValue(binding.bindingName, true);
  }

  Result<bool> Set(const PropertyKey&, Value, Value) override { return false; }

  Result<bool> Delete(const PropertyKey& key) override {
    if (IsSymbolLikeNamespaceKey(key)) return Object::Delete(key);
    TRY(GetModuleExportsList());
    return !isExport(key);
  }

  Result<std::vector<PropertyKey>> OwnPropertyKeys() override {
    TRY(GetModuleExportsList());
    std::vector<PropertyKey> keys;
    for (String* name : exports_) keys.emplace_back(name);
    std::vector<PropertyKey> own = TRY(Object::OwnPropertyKeys());
    for (const PropertyKey& key : own) {
      if (key.isSymbol()) keys.push_back(key);
    }
    return keys;
  }

  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(module_);
    for (String* name : exports_) tracer.visit(reinterpret_cast<Cell*>(name));
  }

 private:
  // IsSymbolLikeNamespaceKey: a Symbol, or "then" on a deferred namespace (so it is never thenable).
  bool IsSymbolLikeNamespaceKey(const PropertyKey& key) const {
    if (key.isSymbol()) return true;
    if (!deferred_) return false;
    Value name = key.toValue();
    return name.isString() && name.asString()->view() == u"then";
  }

  // GetModuleExportsList (its evaluation side effect; the list is exports_).
  Result<void> GetModuleExportsList() {
    if (deferred_) TRY(EvaluateModuleSync(module_));
    return {};
  }

  bool isExport(const PropertyKey& key) const {
    Value name = key.toValue();
    for (String* e : exports_) {
      if (equals(e, name.asString())) return true;
    }
    return false;
  }

  ModuleRecord* module_;
  std::vector<String*> exports_;  // sorted by code unit order
  bool deferred_;                 // [[Deferred]]
};

// 16.2.1.10 GetModuleNamespace(module, phase)
Object* GetModuleNamespace(ModuleRecord* module, NamespacePhase phase) {
  bool deferred = phase == NamespacePhase::Defer;
  Object*& slot = deferred ? module->deferredNamespaceObject : module->namespaceObject;
  if (slot) return slot;
  std::vector<ModuleRecord*> exportStarSet;
  std::vector<std::u16string> exportedNames = GetExportedNames(module, exportStarSet);
  std::vector<std::u16string> unambiguousNames;
  for (const auto& name : exportedNames) {
    if (deferred && name == u"then") continue;
    if (ResolveExport(module, name).kind == ResolvedBinding::Kind::Binding) unambiguousNames.push_back(name);
  }
  std::sort(unambiguousNames.begin(), unambiguousNames.end());
  std::vector<String*> exports;
  for (const auto& name : unambiguousNames) exports.push_back(atom(name));
  auto* ns = snowman::make<ModuleNamespace>(module, std::move(exports), deferred);
  ns->properties().put(PropertyKey(vm().symbols.toStringTag),
                       PropertySlot::data(Value(atom(deferred ? u"Deferred Module" : u"Module")), Attr::None));
  slot = ns;
  return ns;
}

// 16.2.1.6.4 InitializeEnvironment
Result<void> InitializeEnvironment(ModuleRecord* module) {
  Realm* realm = module->realm;
  auto* env = snowman::make<ModuleEnvironment>(globalEnvironment(realm));
  module->environment = env;
  if (module->isSynthetic()) {
    env->createInitializedBinding(atom(u"default"), module->syntheticDefault);
    return {};
  }
  const ast::Program* code = module->code;
  for (const auto& e : code->indirectExportEntries) {
    ResolvedBinding resolution = ResolveExport(module, *e.exportName);
    if (resolution.kind != ResolvedBinding::Kind::Binding) {
      return ThrowSyntaxError("The requested module does not provide an export named '" + utf16ToUtf8(*e.exportName) +
                              "'" + (resolution.kind == ResolvedBinding::Kind::Ambiguous ? " (ambiguous)" : ""));
    }
  }
  for (const auto& in : code->importEntries) {
    ModuleRecord* importedModule = GetImportedModule(module, in.moduleRequest);
    String* localName = atom(in.localName);
    if (in.isNamespaceObject) {
      bool deferred = requestPhase(module, in.moduleRequest) == ast::ImportPhase::Defer;
      Object* ns = GetModuleNamespace(importedModule, deferred ? NamespacePhase::Defer : NamespacePhase::Evaluation);
      env->createInitializedBinding(localName, Value(ns), false);
      continue;
    }
    if (in.isSource) {
      if (!importedModule->moduleSource) {
        return ThrowSyntaxError("Cannot import the source phase of a module that has no module source");
      }
      env->createInitializedBinding(localName, Value(importedModule->moduleSource), false);
      continue;
    }
    ResolvedBinding resolution = ResolveExport(importedModule, in.importName);
    if (resolution.kind != ResolvedBinding::Kind::Binding) {
      return ThrowSyntaxError("The requested module does not provide an export named '" + utf16ToUtf8(in.importName) +
                              "'" + (resolution.kind == ResolvedBinding::Kind::Ambiguous ? " (ambiguous)" : ""));
    }
    if (!resolution.bindingName) {
      Object* value = specialBindingValue(resolution);
      if (!value) return ThrowSyntaxError("Cannot import the source phase of a module that has no module source");
      env->createInitializedBinding(localName, Value(value), false);
    } else {
      env->CreateImportBinding(localName, resolution.module, resolution.bindingName);
    }
  }
  InterpContext moduleContext;
  moduleContext.realm = realm;
  moduleContext.scriptOrModule = module;
  moduleContext.variableEnvironment = env;
  moduleContext.lexicalEnvironment = env;
  moduleContext.strict = true;
  ContextScope scope(&moduleContext);
  for (const auto& dn : code->varNames) {
    String* name = atom(dn);
    if (env->hasBindingDirect(name)) continue;
    env->createInitializedBinding(name, Value::undefined());
  }
  for (const auto& d : code->lexicallyScopedDeclarations) {
    for (const auto& dn : d.boundNames) {
      String* name = atom(dn);
      if (d.isConstant) {
        TRY(env->CreateImmutableBinding(name, true));
      } else {
        TRY(env->CreateMutableBinding(name, false));
      }
      if (d.node->type == ast::NodeType::FunctionDeclaration) {
        Value fo = TRY(InstantiateFunctionObject(d.node->as<ast::FunctionDeclaration>(), env, nullptr));
        TRY(env->InitializeBinding(name, fo));
      }
    }
  }
  return {};
}

// 16.2.1.5.1.1 InnerModuleLinking
Result<uint32_t> InnerModuleLinking(ModuleRecord* module, std::vector<ModuleRecord*>& stack, uint32_t index) {
  using S = ModuleRecord::Status;
  if (module->status == S::Linking || module->status == S::Linked || module->status == S::EvaluatingAsync ||
      module->status == S::Evaluated) {
    return index;
  }
  module->status = S::Linking;
  module->dfsIndex = index;
  module->dfsAncestorIndex = index;
  index++;
  stack.push_back(module);
  for (size_t i = 0; i < module->loadedModules.size(); i++) {
    // Source phase requests are not linked (nor evaluated).
    if (requestPhase(module, uint32_t(i)) == ast::ImportPhase::Source) continue;
    ModuleRecord* requiredModule = module->loadedModules[i];
    index = TRY(InnerModuleLinking(requiredModule, stack, index));
    if (requiredModule->status == S::Linking) {
      module->dfsAncestorIndex = std::min(module->dfsAncestorIndex, requiredModule->dfsAncestorIndex);
    }
  }
  TRY(InitializeEnvironment(module));
  if (module->dfsAncestorIndex == module->dfsIndex) {
    while (true) {
      ModuleRecord* requiredModule = stack.back();
      stack.pop_back();
      requiredModule->status = S::Linked;
      if (requiredModule == module) break;
    }
  }
  return index;
}

// 16.2.1.5.1 Link()
Result<void> Link(ModuleRecord* module) {
  std::vector<ModuleRecord*> stack;
  Result<uint32_t> result = InnerModuleLinking(module, stack, 0);
  if (result.isThrow()) {
    for (ModuleRecord* m : stack) {
      m->status = ModuleRecord::Status::Unlinked;
      m->environment = nullptr;
    }
    return Throw{result.thrown()};
  }
  return {};
}

// The statements of a module body (Evaluation of ModuleBody), then DisposeResources for its top-level
// `using` / `await using` declarations.
Result<Value> evaluateModuleBody(ModuleRecord* module) {
  TRY(DisposeResources(module->environment, evaluateStatementList(module->code->body)));
  return Value::undefined();
}

Result<Value> moduleNativeBody(SuspendableObject* self) {
  return evaluateModuleBody(static_cast<ModuleRecord*>(self->nativeData));
}

// 16.2.1.6.5 ExecuteModule([capability])
Result<void> ExecuteModule(ModuleRecord* module, PromiseCapability* capability = nullptr) {
  if (module->isSynthetic()) return {};
  InterpContext moduleContext;
  moduleContext.realm = module->realm;
  moduleContext.scriptOrModule = module;
  moduleContext.variableEnvironment = module->environment;
  moduleContext.lexicalEnvironment = module->environment;
  moduleContext.strict = true;
  if (!module->hasTLA) {
    ContextScope scope(&moduleContext);
    TRY(evaluateModuleBody(module));
    return {};
  }
  AsyncNativeBlockStart(capability, moduleNativeBody, module, moduleContext);
  return {};
}

thread_local uint64_t gModuleAsyncEvaluationCount = 1;

void ExecuteAsyncModule(ModuleRecord* module);

// 16.2.1.5.3.3 GatherAvailableAncestors
void GatherAvailableAncestors(ModuleRecord* module, std::vector<ModuleRecord*>& execList) {
  for (ModuleRecord* m : module->asyncParentModules) {
    if (std::find(execList.begin(), execList.end(), m) != execList.end()) continue;
    if (m->cycleRoot->hasEvaluationError) continue;
    m->pendingAsyncDependencies--;
    if (m->pendingAsyncDependencies == 0) {
      execList.push_back(m);
      if (!m->hasTLA) GatherAvailableAncestors(m, execList);
    }
  }
}

// 16.2.1.5.3.5 AsyncModuleExecutionRejected
void AsyncModuleExecutionRejected(ModuleRecord* module, Value error) {
  if (module->status == ModuleRecord::Status::Evaluated) return;
  module->hasEvaluationError = true;
  module->evaluationError = error;
  module->status = ModuleRecord::Status::Evaluated;
  module->asyncEvaluation = false;
  // Leaf to root: the module's own top-level promise rejects before its ancestors'.
  if (module->topLevelCapability) (void)rejectCapability(module->topLevelCapability, error);
  for (ModuleRecord* m : module->asyncParentModules) AsyncModuleExecutionRejected(m, error);
}

// 16.2.1.5.3.4 AsyncModuleExecutionFulfilled
void AsyncModuleExecutionFulfilled(ModuleRecord* module) {
  if (module->status == ModuleRecord::Status::Evaluated) return;
  module->asyncEvaluation = false;
  module->status = ModuleRecord::Status::Evaluated;
  if (module->topLevelCapability) (void)resolveCapability(module->topLevelCapability, Value::undefined());
  std::vector<ModuleRecord*> execList;
  GatherAvailableAncestors(module, execList);
  std::stable_sort(execList.begin(), execList.end(), [](ModuleRecord* a, ModuleRecord* b) {
    return a->asyncEvaluationOrder < b->asyncEvaluationOrder;
  });
  for (ModuleRecord* m : execList) {
    if (m->status == ModuleRecord::Status::Evaluated) continue;
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
    m->status = ModuleRecord::Status::Evaluated;
    if (m->topLevelCapability) (void)resolveCapability(m->topLevelCapability, Value::undefined());
  }
}

// A module held in a closure's slot (an object wrapper, since slots hold Values).
class ModuleHolder final : public Object {
 public:
  explicit ModuleHolder(ModuleRecord* module) : Object(nullptr), module(module) {}
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(module);
  }
  ModuleRecord* module;
};

ModuleRecord* heldModule(NativeCall& call) { return call.callee->slots[0].asObject()->as<ModuleHolder>()->module; }

Result<Value> asyncModuleFulfilled(NativeCall& call) {
  AsyncModuleExecutionFulfilled(heldModule(call));
  return Value::undefined();
}

Result<Value> asyncModuleRejected(NativeCall& call) {
  AsyncModuleExecutionRejected(heldModule(call), call.args[0]);
  return Value::undefined();
}

BuiltinFunction* moduleClosure(NativeFn fn, int length, ModuleRecord* module) {
  BuiltinFunction* function = createBuiltinFunction(*vm().currentRealm(), fn, length, PropertyKey(emptyString()));
  function->slots.push_back(Value(snowman::make<ModuleHolder>(module)));
  return function;
}

PromiseCapability* intrinsicCapability() {
  return NewPromiseCapability(Value(vm().currentRealm()->intrinsic(Intrinsic::Promise))).release();
}

// 16.2.1.5.3.2 ExecuteAsyncModule
void ExecuteAsyncModule(ModuleRecord* module) {
  PromiseCapability* capability = intrinsicCapability();
  BuiltinFunction* onFulfilled = moduleClosure(asyncModuleFulfilled, 0, module);
  BuiltinFunction* onRejected = moduleClosure(asyncModuleRejected, 0, module);
  PerformPromiseThen(capability->promise.asObject()->as<PromiseObject>(), Value(onFulfilled), Value(onRejected));
  (void)ExecuteModule(module, capability);
}

// IsModuleSCCEvaluated (import defer)
bool IsModuleSCCEvaluated(const ModuleRecord* module) {
  if (module->cycleRoot) return module->cycleRoot->status == ModuleRecord::Status::Evaluated;
  return module->status == ModuleRecord::Status::Evaluated;
}

// GatherAsynchronousTransitiveDependencies (import defer): the asynchronous, not yet evaluated modules a deferred
// import of `module` evaluates eagerly, in post order, stopping each branch at an asynchronous module.
void GatherAsynchronousTransitiveDependencies(ModuleRecord* module, std::vector<ModuleRecord*>& seen,
                                              std::vector<ModuleRecord*>& result) {
  if (std::find(seen.begin(), seen.end(), module) != seen.end()) return;
  seen.push_back(module);
  if (module->isSynthetic()) return;
  if (module->status == ModuleRecord::Status::Evaluating || IsModuleSCCEvaluated(module)) return;
  if (module->hasTLA) {
    if (std::find(result.begin(), result.end(), module) == result.end()) result.push_back(module);
    return;
  }
  for (size_t i = 0; i < module->loadedModules.size(); i++) {
    if (requestPhase(module, uint32_t(i)) == ast::ImportPhase::Source) continue;
    GatherAsynchronousTransitiveDependencies(module->loadedModules[i], seen, result);
  }
}

std::vector<ModuleRecord*> GatherAsynchronousTransitiveDependencies(ModuleRecord* module) {
  std::vector<ModuleRecord*> seen, result;
  GatherAsynchronousTransitiveDependencies(module, seen, result);
  return result;
}

// ReadyForSyncExecution (import defer)
bool ReadyForSyncExecution(ModuleRecord* module, std::vector<ModuleRecord*>& seen) {
  using S = ModuleRecord::Status;
  if (module->isSynthetic()) return true;
  if (std::find(seen.begin(), seen.end(), module) != seen.end()) return true;
  seen.push_back(module);
  if (IsModuleSCCEvaluated(module)) return true;
  if (module->status == S::Evaluating || module->status == S::EvaluatingAsync) return false;
  if (module->hasTLA) return false;
  for (size_t i = 0; i < module->loadedModules.size(); i++) {
    if (requestPhase(module, uint32_t(i)) == ast::ImportPhase::Source) continue;
    if (!ReadyForSyncExecution(module->loadedModules[i], seen)) return false;
  }
  return true;
}

Value Evaluate(ModuleRecord* module);

// EvaluateModuleSync (import defer): evaluate `module` now; a TypeError if that would not settle synchronously.
Result<void> EvaluateModuleSync(ModuleRecord* module) {
  std::vector<ModuleRecord*> seen;
  if (!ReadyForSyncExecution(module, seen)) {
    return ThrowTypeError("Cannot evaluate a deferred module synchronously while it or a dependency is evaluating "
                          "or uses top-level await");
  }
  auto* promise = Evaluate(module).asObject()->as<PromiseObject>();
  if (promise->state == PromiseObject::State::Rejected) {
    promise->isHandled = true;
    return Throw{promise->result};
  }
  return {};
}

// 16.2.1.5.3.1 InnerModuleEvaluation
Result<uint32_t> InnerModuleEvaluation(ModuleRecord* module, std::vector<ModuleRecord*>& stack, uint32_t index) {
  using S = ModuleRecord::Status;
  if (module->status == S::EvaluatingAsync || module->status == S::Evaluated) {
    if (!module->hasEvaluationError) return index;
    return Throw{module->evaluationError};
  }
  if (module->status == S::Evaluating) return index;
  module->status = S::Evaluating;
  module->dfsIndex = index;
  module->dfsAncestorIndex = index;
  module->pendingAsyncDependencies = 0;
  index++;
  // The evaluation list: each evaluation-phase dependency, and for a deferred one only its asynchronous transitive
  // dependencies (GatherAsynchronousTransitiveDependencies); source phase requests are not evaluated.
  std::vector<ModuleRecord*> evaluationList;
  auto addToEvaluationList = [&](ModuleRecord* m) {
    if (std::find(evaluationList.begin(), evaluationList.end(), m) == evaluationList.end()) evaluationList.push_back(m);
  };
  for (size_t i = 0; i < module->loadedModules.size(); i++) {
    ast::ImportPhase phase = requestPhase(module, uint32_t(i));
    if (phase == ast::ImportPhase::Source) continue;
    if (phase == ast::ImportPhase::Defer) {
      for (ModuleRecord* m : GatherAsynchronousTransitiveDependencies(module->loadedModules[i])) addToEvaluationList(m);
    } else {
      addToEvaluationList(module->loadedModules[i]);
    }
  }
  stack.push_back(module);
  for (ModuleRecord* requiredModule : evaluationList) {
    index = TRY(InnerModuleEvaluation(requiredModule, stack, index));
    if (requiredModule->status == S::Evaluating) {
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
    module->asyncEvaluationOrder = gModuleAsyncEvaluationCount++;
    if (module->pendingAsyncDependencies == 0) ExecuteAsyncModule(module);
  } else {
    TRY(ExecuteModule(module));
  }
  if (module->dfsAncestorIndex == module->dfsIndex) {
    while (true) {
      ModuleRecord* requiredModule = stack.back();
      stack.pop_back();
      requiredModule->status = requiredModule->asyncEvaluation ? S::EvaluatingAsync : S::Evaluated;
      requiredModule->cycleRoot = module;
      if (requiredModule == module) break;
    }
  }
  return index;
}

// 16.2.1.5.3 Evaluate(): the module's promise.
Value Evaluate(ModuleRecord* module) {
  using S = ModuleRecord::Status;
  if ((module->status == S::EvaluatingAsync || module->status == S::Evaluated) && module->cycleRoot) {
    module = module->cycleRoot;
  }
  if (module->topLevelCapability) return module->topLevelCapability->promise;
  std::vector<ModuleRecord*> stack;
  PromiseCapability* capability = intrinsicCapability();
  module->topLevelCapability = capability;
  Result<uint32_t> result = InnerModuleEvaluation(module, stack, 0);
  if (result.isThrow()) {
    for (ModuleRecord* m : stack) {
      m->status = S::Evaluated;
      m->hasEvaluationError = true;
      m->evaluationError = result.thrown();
    }
    (void)rejectCapability(capability, result.thrown());
  } else if (!module->asyncEvaluation) {
    (void)resolveCapability(capability, Value::undefined());
  }
  return capability->promise;
}

// The directory and realm a referrer (Script Record, Module Record, or none) loads modules for.
void referrerLocation(Cell* referrer, std::string& directory, Realm*& realm) {
  realm = vm().currentRealm();
  directory = gScriptDirectory;
  if (auto* module = dynamic_cast<ModuleRecord*>(referrer)) {
    directory = directoryOf(module->path);
    realm = module->realm;
  } else if (auto* script = dynamic_cast<ScriptRecord*>(referrer)) {
    realm = script->realm;
  }
}

// slots: [capability]
Result<Value> dynamicImportRejected(NativeCall& call) {
  (void)rejectCapability(call.callee->slots[0].asObject()->as<PromiseCapability>(), call.args[0]);
  return Value::undefined();
}

// slots: [module holder, capability, phase (0 evaluation, 1 defer)]
Result<Value> dynamicImportEvaluated(NativeCall& call) {
  ModuleRecord* module = heldModule(call);
  auto* capability = call.callee->slots[1].asObject()->as<PromiseCapability>();
  bool deferred = call.callee->slots[2].asNumber() == 1;
  Object* ns = GetModuleNamespace(module, deferred ? NamespacePhase::Defer : NamespacePhase::Evaluation);
  (void)resolveCapability(capability, Value(ns));
  return Value::undefined();
}

// The countdown of SafePerformPromiseAll: a cell in a closure slot.
class PendingCount final : public Object {
 public:
  explicit PendingCount(size_t count) : Object(nullptr), count(count) {}
  size_t count;
};

// slots: [PendingCount, onFulfilled]
Result<Value> allFulfilledStep(NativeCall& call) {
  auto* pending = call.callee->slots[0].asObject()->as<PendingCount>();
  if (pending->count > 0 && --pending->count == 0) TRY(Call(call.callee->slots[1], Value::undefined(), {}));
  return Value::undefined();
}

// SafePerformPromiseAll(promises) followed by PerformPromiseThen(..., onFulfilled, onRejected): onFulfilled once
// every promise has fulfilled, onRejected with the first rejection.
void whenAllFulfilled(const ValueVector& promises, Value onFulfilled, Value onRejected) {
  BuiltinFunction* step = createBuiltinFunction(*vm().currentRealm(), allFulfilledStep, 1, PropertyKey(emptyString()));
  step->slots.push_back(Value(snowman::make<PendingCount>(promises.size())));
  step->slots.push_back(onFulfilled);
  for (Value promise : promises) PerformPromiseThen(promise.asObject()->as<PromiseObject>(), Value(step), onRejected);
}

// slots: [module holder, capability, onRejected, phase (0 evaluation, 1 defer)]
Result<Value> dynamicImportLinkAndEvaluate(NativeCall& call) {
  ModuleRecord* module = heldModule(call);
  auto* capability = call.callee->slots[1].asObject()->as<PromiseCapability>();
  Value phase = call.callee->slots[3];
  Result<void> link = Link(module);
  if (link.isThrow()) {
    (void)rejectCapability(capability, link.thrown());
    return Value::undefined();
  }
  BuiltinFunction* onFulfilled = moduleClosure(dynamicImportEvaluated, 0, module);
  onFulfilled->slots.push_back(Value(capability));
  onFulfilled->slots.push_back(phase);
  if (phase.asNumber() == 1) {
    // import.defer(): evaluate only the asynchronous transitive dependencies now.
    std::vector<ModuleRecord*> evaluationList = GatherAsynchronousTransitiveDependencies(module);
    if (evaluationList.empty()) return Call(Value(onFulfilled), Value::undefined(), {});
    ValueVector promises;
    for (ModuleRecord* dep : evaluationList) promises.push_back(Evaluate(dep));
    whenAllFulfilled(promises, Value(onFulfilled), call.callee->slots[2]);
    return Value::undefined();
  }
  Value evaluatePromise = Evaluate(module);
  PerformPromiseThen(evaluatePromise.asObject()->as<PromiseObject>(), Value(onFulfilled), call.callee->slots[2]);
  return Value::undefined();
}

// ContinueDynamicImport(promiseCapability, moduleCompletion, phase)
void ContinueDynamicImport(PromiseCapability* promiseCapability, Result<ModuleRecord*> moduleCompletion,
                           ast::ImportPhase phase = ast::ImportPhase::Evaluation) {
  if (moduleCompletion.isThrow()) {
    (void)rejectCapability(promiseCapability, moduleCompletion.thrown());
    return;
  }
  ModuleRecord* module = moduleCompletion.value();
  if (phase == ast::ImportPhase::Source) {
    if (!module->moduleSource) {
      Value error(CreateError(vm().currentRealm(), Intrinsic::SyntaxErrorPrototype,
                              "Cannot import the source phase of a module that has no module source"));
      (void)rejectCapability(promiseCapability, error);
    } else {
      (void)resolveCapability(promiseCapability, Value(module->moduleSource));
    }
    return;
  }
  PromiseCapability* loadCapability = intrinsicCapability();
  Result<void> loaded = loadRequestedModules(module);
  if (loaded.isThrow()) {
    (void)rejectCapability(loadCapability, loaded.thrown());
  } else {
    (void)resolveCapability(loadCapability, Value::undefined());
  }
  BuiltinFunction* onRejected = createBuiltinFunction(*vm().currentRealm(), dynamicImportRejected, 1, PropertyKey(emptyString()));
  onRejected->slots.push_back(Value(promiseCapability));
  BuiltinFunction* linkAndEvaluate = moduleClosure(dynamicImportLinkAndEvaluate, 0, module);
  linkAndEvaluate->slots.push_back(Value(promiseCapability));
  linkAndEvaluate->slots.push_back(Value(onRejected));
  linkAndEvaluate->slots.push_back(Value::number(phase == ast::ImportPhase::Defer ? 1 : 0));
  PerformPromiseThen(loadCapability->promise.asObject()->as<PromiseObject>(), Value(linkAndEvaluate), Value(onRejected));
}

Value rejectedWithTypeError(PromiseCapability* capability, const char* message) {
  (void)rejectCapability(capability, Value(CreateError(vm().currentRealm(), Intrinsic::TypeErrorPrototype, message)));
  return capability->promise;
}

}  // namespace

// 13.3.10.2 EvaluateImportCall
Result<Value> evaluateImportCall(const ast::ImportExpression* node) {
  Cell* referrer = GetActiveScriptOrModule();
  Value specifier = TRY(evaluate(node->source));
  Value options = Value::undefined();
  if (node->options) options = TRY(evaluate(node->options));
  PromiseCapability* promiseCapability = intrinsicCapability();
  Result<String*> specifierString = ToString(specifier);
  if (specifierString.isThrow()) {
    (void)rejectCapability(promiseCapability, specifierString.thrown());
    return promiseCapability->promise;
  }
  ast::ModuleRequest request;
  request.specifier = std::u16string(specifierString.value()->view());
  request.phase = node->phase;
  if (!options.isUndefined()) {
    if (!options.isObject()) return rejectedWithTypeError(promiseCapability, "The second argument of import() must be an object");
    Result<Value> attributesObj = Get(options.asObject(), PropertyKey(u"with"));
    if (attributesObj.isThrow()) {
      (void)rejectCapability(promiseCapability, attributesObj.thrown());
      return promiseCapability->promise;
    }
    if (!attributesObj.value().isUndefined()) {
      if (!attributesObj.value().isObject()) return rejectedWithTypeError(promiseCapability, "The 'with' option must be an object");
      Result<ValueVector> entries = EnumerableOwnProperties(attributesObj.value().asObject(), EnumerableOwnKind::KeyValue);
      if (entries.isThrow()) {
        (void)rejectCapability(promiseCapability, entries.thrown());
        return promiseCapability->promise;
      }
      for (Value entry : entries.value()) {
        Value key = Get(entry.asObject(), PropertyKey::index(0)).release();
        Value value = Get(entry.asObject(), PropertyKey::index(1)).release();
        if (!key.isString()) continue;
        if (!value.isString()) return rejectedWithTypeError(promiseCapability, "Import attribute values must be strings");
        request.attributes.push_back({std::u16string(key.asString()->view()), std::u16string(value.asString()->view())});
      }
      for (const auto& attribute : request.attributes) {
        if (attribute.key != u"type") return rejectedWithTypeError(promiseCapability, "Unsupported import attribute");
      }
    }
  }
  std::string directory;
  Realm* realm;
  referrerLocation(referrer, directory, realm);
  ContinueDynamicImport(promiseCapability, hostLoadImportedModule(realm, directory, request), request.phase);
  return promiseCapability->promise;
}

void ImportModuleForRealm(Realm* realm, String* specifier, PromiseCapability* capability) {
  ast::ModuleRequest request;
  request.specifier = std::u16string(specifier->view());
  ContinueDynamicImport(capability, hostLoadImportedModule(realm, gScriptDirectory, request));
}

// 13.3.12.1 import.meta
Result<Value> evaluateImportMeta() {
  auto* module = dynamic_cast<ModuleRecord*>(GetActiveScriptOrModule());
  if (!module) return ThrowSyntaxError("import.meta is only valid in module code");
  if (!module->importMeta) module->importMeta = OrdinaryObjectCreate(nullptr);
  return Value(module->importMeta);
}

Result<void> runModuleFile(Realm* realm, const std::string& path, std::u16string source) {
  std::string resolved = resolveSpecifier(".", path.starts_with("/") ? path : "./" + path).value();
  setScriptDirectory(directoryOf(resolved));
  ModuleRecord* module = TRY(createModule(realm, resolved, std::move(source), ModuleType::JavaScript));
  realmState(realm)->moduleMap.emplace_back(resolved, module);
  ExecutionContext context;
  context.realm = realm;
  ContextScope scope(&context);
  TRY(loadRequestedModules(module));
  TRY(Link(module));
  Value promise = Evaluate(module);
  TRY(vm().runJobs());
  auto* evaluated = promise.asObject()->as<PromiseObject>();
  if (evaluated->state == PromiseObject::State::Rejected) return Throw{evaluated->result};
  if (evaluated->state == PromiseObject::State::Pending) return ThrowError(Intrinsic::ErrorPrototype, "module evaluation did not complete");
  return {};
}

namespace {

}  // namespace

void setScriptDirectory(const std::string& directory) { gScriptDirectory = directory; }

}  // namespace snowman::interp
