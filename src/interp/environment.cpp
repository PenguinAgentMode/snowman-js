// Environment Records (9.1) and PrivateEnvironment Records (9.2).
#include "interp/environment.h"

#include "interp/function.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/vm.h"

namespace snowman::interp {

namespace {

constexpr size_t kIndexThreshold = 12;

Throw throwNotDefined(String* name) {
  return ThrowReferenceError(name->toUtf8() + " is not defined");
}

Throw throwUninitialized(String* name) {
  return ThrowReferenceError("Cannot access '" + name->toUtf8() + "' before initialization");
}

}  // namespace

void Environment::trace(Tracer& tracer) { tracer.visit(outer); }

Environment* GetThisEnvironment(Environment* env) {
  while (!env->HasThisBinding()) env = env->outer;
  return env;
}

// ---- Declarative ----

DeclarativeEnvironment::Binding* DeclarativeEnvironment::find(String* name) {
  if (index_) {
    auto it = index_->find(name);
    return it == index_->end() ? nullptr : &bindings_[it->second];
  }
  for (auto& binding : bindings_) {
    if (binding.name == name) return &binding;
  }
  return nullptr;
}

DeclarativeEnvironment::Binding& DeclarativeEnvironment::add(String* name) {
  bindings_.push_back(Binding{name, Value::empty(), true, false, false});
  if (index_) {
    (*index_)[name] = uint32_t(bindings_.size() - 1);
  } else if (bindings_.size() > kIndexThreshold) {
    index_ = std::make_unique<std::unordered_map<String*, uint32_t>>();
    for (uint32_t i = 0; i < bindings_.size(); i++) (*index_)[bindings_[i].name] = i;
  }
  return bindings_.back();
}

void DeclarativeEnvironment::createInitializedBinding(String* name, Value value, bool mutable_) {
  Binding& binding = add(name);
  binding.value = value;
  binding.mutable_ = mutable_;
  binding.strict = true;
}

Result<bool> DeclarativeEnvironment::HasBinding(String* name) { return find(name) != nullptr; }

Result<void> DeclarativeEnvironment::CreateMutableBinding(String* name, bool deletable) {
  Binding& binding = add(name);
  binding.deletable = deletable;
  return {};
}

Result<void> DeclarativeEnvironment::CreateImmutableBinding(String* name, bool strict) {
  Binding& binding = add(name);
  binding.mutable_ = false;
  binding.strict = strict;
  return {};
}

Result<void> DeclarativeEnvironment::InitializeBinding(String* name, Value value) {
  find(name)->value = value;
  return {};
}

Result<void> DeclarativeEnvironment::SetMutableBinding(String* name, Value value, bool strict) {
  Binding* binding = find(name);
  if (!binding) {
    if (strict) return throwNotDefined(name);
    TRY(CreateMutableBinding(name, true));
    TRY(InitializeBinding(name, value));
    return {};
  }
  if (binding->strict) strict = true;
  if (binding->value.isEmpty()) return throwUninitialized(name);
  if (binding->mutable_) {
    binding->value = value;
    return {};
  }
  if (strict) return ThrowTypeError("Assignment to constant variable '" + name->toUtf8() + "'");
  return {};
}

Result<Value> DeclarativeEnvironment::GetBindingValue(String* name, bool) {
  Binding* binding = find(name);
  if (binding->value.isEmpty()) return throwUninitialized(name);
  return binding->value;
}

Result<bool> DeclarativeEnvironment::DeleteBinding(String* name) {
  Binding* binding = find(name);
  if (!binding->deletable) return false;
  size_t at = size_t(binding - bindings_.data());
  bindings_.erase(bindings_.begin() + ptrdiff_t(at));
  if (index_) {
    index_->clear();
    for (uint32_t i = 0; i < bindings_.size(); i++) (*index_)[bindings_[i].name] = i;
  }
  return true;
}

void DeclarativeEnvironment::trace(Tracer& tracer) {
  Environment::trace(tracer);
  for (auto& binding : bindings_) {
    tracer.visit(reinterpret_cast<Cell*>(binding.name));
    tracer.visit(binding.value);
    tracer.visit(reinterpret_cast<Cell*>(binding.indirectModule));
    tracer.visit(reinterpret_cast<Cell*>(binding.indirectName));
  }
}

// ---- Function ----

FunctionEnvironment::FunctionEnvironment(Object* function, Value newTarget, Environment* outer, bool lexicalThis)
    : DeclarativeEnvironment(Kind::Function, outer),
      thisStatus(lexicalThis ? ThisStatus::Lexical : ThisStatus::Uninitialized),
      functionObject(function),
      newTarget(newTarget) {}

bool FunctionEnvironment::HasSuperBinding() const {
  if (thisStatus == ThisStatus::Lexical) return false;
  return homeObjectOf(functionObject) != nullptr;
}

Result<Value> FunctionEnvironment::BindThisValue(Value value) {
  if (thisStatus == ThisStatus::Initialized) return ThrowReferenceError("'this' is already initialized");
  thisValue = value;
  thisStatus = ThisStatus::Initialized;
  return value;
}

Result<Value> FunctionEnvironment::GetThisBinding() {
  if (thisStatus == ThisStatus::Uninitialized) {
    return ThrowReferenceError("Must call super constructor before accessing 'this'");
  }
  return thisValue;
}

Result<Value> FunctionEnvironment::GetSuperBase() {
  Object* home = homeObjectOf(functionObject);
  if (!home) return Value::undefined();
  Object* prototype = TRY(home->GetPrototypeOf());
  return prototype ? Value(prototype) : Value::null();
}

void FunctionEnvironment::trace(Tracer& tracer) {
  DeclarativeEnvironment::trace(tracer);
  tracer.visit(thisValue);
  tracer.visit(functionObject);
  tracer.visit(newTarget);
}

// ---- Object ----

Result<bool> ObjectEnvironment::HasBinding(String* name) {
  bool found = TRY(HasProperty(bindingObject, PropertyKey(name)));
  if (!found) return false;
  if (!isWithEnvironment) return true;
  Value unscopables = TRY(Get(bindingObject, PropertyKey(vm().symbols.unscopables)));
  if (unscopables.isObject()) {
    bool blocked = ToBoolean(TRY(Get(unscopables.asObject(), PropertyKey(name))));
    if (blocked) return false;
  }
  return true;
}

Result<void> ObjectEnvironment::CreateMutableBinding(String* name, bool deletable) {
  return DefinePropertyOrThrow(
      bindingObject, PropertyKey(name),
      PropertyDescriptor::data(Value::undefined(), deletable ? Attr::All : Attr::Writable | Attr::Enumerable));
}

Result<void> ObjectEnvironment::CreateImmutableBinding(String*, bool) { return {}; }  // never used

Result<void> ObjectEnvironment::InitializeBinding(String* name, Value value) {
  return SetMutableBinding(name, value, false);
}

Result<void> ObjectEnvironment::SetMutableBinding(String* name, Value value, bool strict) {
  PropertyKey key(name);
  bool stillExists = TRY(HasProperty(bindingObject, key));
  if (!stillExists && strict) return throwNotDefined(name);
  return Set(bindingObject, key, value, strict);
}

Result<Value> ObjectEnvironment::GetBindingValue(String* name, bool strict) {
  PropertyKey key(name);
  bool value = TRY(HasProperty(bindingObject, key));
  if (!value) {
    if (!strict) return Value::undefined();
    return throwNotDefined(name);
  }
  return Get(bindingObject, key);
}

Result<bool> ObjectEnvironment::DeleteBinding(String* name) { return bindingObject->Delete(PropertyKey(name)); }

void ObjectEnvironment::trace(Tracer& tracer) {
  Environment::trace(tracer);
  tracer.visit(bindingObject);
}

// ---- Global ----

GlobalEnvironment::GlobalEnvironment(Object* globalObject, Object* thisValue)
    : Environment(Kind::Global, nullptr),
      objectRecord(make<ObjectEnvironment>(globalObject, false, nullptr)),
      globalThisValue(thisValue),
      declarativeRecord(make<DeclarativeEnvironment>(nullptr)) {}

Result<bool> GlobalEnvironment::HasBinding(String* name) {
  if (declarativeRecord->hasBindingDirect(name)) return true;
  return objectRecord->HasBinding(name);
}

Result<void> GlobalEnvironment::CreateMutableBinding(String* name, bool deletable) {
  if (declarativeRecord->hasBindingDirect(name)) {
    return ThrowTypeError("Identifier '" + name->toUtf8() + "' has already been declared");
  }
  return declarativeRecord->CreateMutableBinding(name, deletable);
}

Result<void> GlobalEnvironment::CreateImmutableBinding(String* name, bool strict) {
  if (declarativeRecord->hasBindingDirect(name)) {
    return ThrowTypeError("Identifier '" + name->toUtf8() + "' has already been declared");
  }
  return declarativeRecord->CreateImmutableBinding(name, strict);
}

Result<void> GlobalEnvironment::InitializeBinding(String* name, Value value) {
  if (declarativeRecord->hasBindingDirect(name)) return declarativeRecord->InitializeBinding(name, value);
  return objectRecord->InitializeBinding(name, value);
}

Result<void> GlobalEnvironment::SetMutableBinding(String* name, Value value, bool strict) {
  if (declarativeRecord->hasBindingDirect(name)) return declarativeRecord->SetMutableBinding(name, value, strict);
  return objectRecord->SetMutableBinding(name, value, strict);
}

Result<Value> GlobalEnvironment::GetBindingValue(String* name, bool strict) {
  if (declarativeRecord->hasBindingDirect(name)) return declarativeRecord->GetBindingValue(name, strict);
  return objectRecord->GetBindingValue(name, strict);
}

Result<bool> GlobalEnvironment::DeleteBinding(String* name) {
  if (declarativeRecord->hasBindingDirect(name)) return declarativeRecord->DeleteBinding(name);
  bool existing = TRY(HasOwnProperty(objectRecord->bindingObject, PropertyKey(name)));
  if (existing) return objectRecord->DeleteBinding(name);
  return true;
}

Result<bool> GlobalEnvironment::HasRestrictedGlobalProperty(String* name) {
  auto existing = TRY(objectRecord->bindingObject->GetOwnProperty(PropertyKey(name)));
  if (!existing) return false;
  return !existing->configurable.value_or(false);
}

Result<bool> GlobalEnvironment::CanDeclareGlobalVar(String* name) {
  Object* global = objectRecord->bindingObject;
  bool hasProperty = TRY(HasOwnProperty(global, PropertyKey(name)));
  if (hasProperty) return true;
  return IsExtensible(global);
}

Result<bool> GlobalEnvironment::CanDeclareGlobalFunction(String* name) {
  Object* global = objectRecord->bindingObject;
  auto existing = TRY(global->GetOwnProperty(PropertyKey(name)));
  if (!existing) return IsExtensible(global);
  if (existing->configurable.value_or(false)) return true;
  return existing->isDataDescriptor() && existing->writable.value_or(false) && existing->enumerable.value_or(false);
}

Result<void> GlobalEnvironment::CreateGlobalVarBinding(String* name, bool deletable) {
  Object* global = objectRecord->bindingObject;
  bool hasProperty = TRY(HasOwnProperty(global, PropertyKey(name)));
  bool extensible = TRY(IsExtensible(global));
  if (!hasProperty && extensible) {
    TRY(objectRecord->CreateMutableBinding(name, deletable));
    TRY(objectRecord->InitializeBinding(name, Value::undefined()));
  }
  return {};
}

Result<void> GlobalEnvironment::CreateGlobalFunctionBinding(String* name, Value value, bool deletable) {
  Object* global = objectRecord->bindingObject;
  PropertyKey key(name);
  auto existing = TRY(global->GetOwnProperty(key));
  PropertyDescriptor desc;
  if (!existing || existing->configurable.value_or(false)) {
    desc = PropertyDescriptor::data(value, deletable ? Attr::All : Attr::Writable | Attr::Enumerable);
  } else {
    desc.value = value;
  }
  TRY(DefinePropertyOrThrow(global, key, desc));
  return Set(global, key, value, false);
}

void GlobalEnvironment::trace(Tracer& tracer) {
  Environment::trace(tracer);
  tracer.visit(objectRecord);
  tracer.visit(globalThisValue);
  tracer.visit(declarativeRecord);
}

// ---- Module ----

Result<Value> ModuleEnvironment::GetBindingValue(String* name, bool strict) {
  Binding* binding = find(name);
  if (binding->indirectModule) {
    Environment* target = moduleEnvironmentOf(binding->indirectModule);
    if (!target) return throwUninitialized(name);
    return target->GetBindingValue(binding->indirectName, true);
  }
  return DeclarativeEnvironment::GetBindingValue(name, strict);
}

void ModuleEnvironment::CreateImportBinding(String* name, ModuleRecord* module, String* targetName) {
  Binding& binding = add(name);
  binding.mutable_ = false;
  binding.strict = true;
  binding.value = Value::undefined();
  binding.indirectModule = module;
  binding.indirectName = targetName;
}

// ---- Private ----

PrivateName* PrivateEnvironment::findOwn(String* identifier) const {
  for (PrivateName* name : names) {
    if (name->description == identifier) return name;
  }
  return nullptr;
}

PrivateName* PrivateEnvironment::resolve(String* identifier) const {
  for (const PrivateEnvironment* env = this; env; env = env->outer) {
    if (PrivateName* name = env->findOwn(identifier)) return name;
  }
  return nullptr;
}

void PrivateEnvironment::trace(Tracer& tracer) {
  tracer.visit(reinterpret_cast<Cell*>(outer));
  for (PrivateName* name : names) tracer.visit(name);
}

}  // namespace snowman::interp
