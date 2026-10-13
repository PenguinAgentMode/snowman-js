// Environment Records (ECMA-262 9.1) and PrivateEnvironment Records (9.2). Binding names are atoms, so names
// compare by pointer.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "runtime/object.h"
#include "runtime/property_map.h"
#include "runtime/result.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace snowman::builtins {
class DisposeCapability;
}

namespace snowman::interp {

class ModuleRecord;

class Environment : public Cell {
 public:
  enum class Kind : uint8_t { Declarative, Function, Module, Object, Global };

  Environment(Kind kind, Environment* outer) : outer(outer), kind_(kind) {}
  Kind kind() const { return kind_; }

  // 9.1.1: the abstract methods of every Environment Record.
  virtual Result<bool> HasBinding(String* name) = 0;
  virtual Result<void> CreateMutableBinding(String* name, bool deletable) = 0;
  virtual Result<void> CreateImmutableBinding(String* name, bool strict) = 0;
  virtual Result<void> InitializeBinding(String* name, Value value) = 0;
  virtual Result<void> SetMutableBinding(String* name, Value value, bool strict) = 0;
  virtual Result<Value> GetBindingValue(String* name, bool strict) = 0;
  virtual Result<bool> DeleteBinding(String* name) = 0;
  virtual bool HasThisBinding() const { return false; }
  virtual bool HasSuperBinding() const { return false; }
  virtual Value WithBaseObject() const { return Value::undefined(); }

  void trace(Tracer& tracer) override;

  Environment* outer;  // [[OuterEnv]], nullptr for the global environment

 private:
  Kind kind_;
};

// 9.1.1.1 Declarative Environment Records.
class DeclarativeEnvironment : public Environment {
 public:
  struct Binding {
    String* name;
    Value value;  // empty: uninitialized (TDZ)
    bool mutable_;
    bool deletable;
    bool strict;  // immutable bindings: a strict binding throws on assignment even in sloppy code
    // Module import bindings: the binding is name `indirectName` of `indirectModule`'s environment.
    ModuleRecord* indirectModule = nullptr;
    String* indirectName = nullptr;
  };

  explicit DeclarativeEnvironment(Environment* outer) : Environment(Kind::Declarative, outer) {}

  Result<bool> HasBinding(String* name) override;
  Result<void> CreateMutableBinding(String* name, bool deletable) override;
  Result<void> CreateImmutableBinding(String* name, bool strict) override;
  Result<void> InitializeBinding(String* name, Value value) override;
  Result<void> SetMutableBinding(String* name, Value value, bool strict) override;
  Result<Value> GetBindingValue(String* name, bool strict) override;
  Result<bool> DeleteBinding(String* name) override;

  // Direct access for the interpreter (no observable behaviour in declarative records).
  Binding* find(String* name);
  bool hasBindingDirect(String* name) { return find(name) != nullptr; }
  // A new binding initialized to `value` (CreateMutableBinding + InitializeBinding).
  void createInitializedBinding(String* name, Value value, bool mutable_ = true);
  const std::vector<Binding>& bindings() const { return bindings_; }
  void reserve(size_t count) { bindings_.reserve(count); }
  // Every binding back to uninitialized (a reused loop iteration environment).
  void resetBindings() {
    for (Binding& binding : bindings_) binding.value = Value::empty();
  }

  // The environment of a catch clause (B.3.4: eval code may redeclare its parameter with var).
  bool isCatchEnvironment = false;
  // [[DisposeCapability]]: created by the first `using` / `await using` binding initialized here.
  builtins::DisposeCapability* disposeCapability = nullptr;

  void trace(Tracer& tracer) override;

 protected:
  DeclarativeEnvironment(Kind kind, Environment* outer) : Environment(kind, outer) {}
  Binding& add(String* name);

  std::vector<Binding> bindings_;
  std::unique_ptr<std::unordered_map<String*, uint32_t>> index_;  // built once a record has many bindings
};

// 9.1.1.3 Function Environment Records.
class FunctionEnvironment final : public DeclarativeEnvironment {
 public:
  enum class ThisStatus : uint8_t { Lexical, Initialized, Uninitialized };

  // NewFunctionEnvironment(F, newTarget): F is an ECMAScriptFunction; newTarget undefined or an object.
  FunctionEnvironment(Object* function, Value newTarget, Environment* outer, bool lexicalThis);

  bool HasThisBinding() const override { return thisStatus != ThisStatus::Lexical; }
  bool HasSuperBinding() const override;
  Result<Value> BindThisValue(Value value);
  Result<Value> GetThisBinding();
  // GetSuperBase: the [[HomeObject]]'s [[GetPrototypeOf]] (undefined when there is no home object).
  Result<Value> GetSuperBase();

  void trace(Tracer& tracer) override;

  Value thisValue;
  ThisStatus thisStatus;
  Object* functionObject;
  Value newTarget;
};

// 9.1.1.2 Object Environment Records.
class ObjectEnvironment final : public Environment {
 public:
  ObjectEnvironment(Object* bindingObject, bool isWithEnvironment, Environment* outer)
      : Environment(Kind::Object, outer), bindingObject(bindingObject), isWithEnvironment(isWithEnvironment) {}

  Result<bool> HasBinding(String* name) override;
  Result<void> CreateMutableBinding(String* name, bool deletable) override;
  Result<void> CreateImmutableBinding(String* name, bool strict) override;
  Result<void> InitializeBinding(String* name, Value value) override;
  Result<void> SetMutableBinding(String* name, Value value, bool strict) override;
  Result<Value> GetBindingValue(String* name, bool strict) override;
  Result<bool> DeleteBinding(String* name) override;
  Value WithBaseObject() const override {
    return isWithEnvironment ? Value(bindingObject) : Value::undefined();
  }
  // The binding object's own data property `name`, when it can be read and written directly (an ordinary
  // binding object, not a with environment); nullopt otherwise or when there is none.
  std::optional<PropertySlot> ownDataSlot(String* name);

  void trace(Tracer& tracer) override;

  Object* bindingObject;
  bool isWithEnvironment;
};

// 9.1.1.4 Global Environment Records.
class GlobalEnvironment final : public Environment {
 public:
  GlobalEnvironment(Object* globalObject, Object* thisValue);

  Result<bool> HasBinding(String* name) override;
  Result<void> CreateMutableBinding(String* name, bool deletable) override;
  Result<void> CreateImmutableBinding(String* name, bool strict) override;
  Result<void> InitializeBinding(String* name, Value value) override;
  Result<void> SetMutableBinding(String* name, Value value, bool strict) override;
  Result<Value> GetBindingValue(String* name, bool strict) override;
  Result<bool> DeleteBinding(String* name) override;
  bool HasThisBinding() const override { return true; }

  Value GetThisBinding() const { return globalThisValue; }
  bool HasLexicalDeclaration(String* name) { return declarativeRecord->hasBindingDirect(name); }
  Result<bool> HasRestrictedGlobalProperty(String* name);
  Result<bool> CanDeclareGlobalVar(String* name);
  Result<bool> CanDeclareGlobalFunction(String* name);
  Result<void> CreateGlobalVarBinding(String* name, bool deletable);
  Result<void> CreateGlobalFunctionBinding(String* name, Value value, bool deletable);

  void trace(Tracer& tracer) override;

  ObjectEnvironment* objectRecord;
  Object* globalThisValue;
  DeclarativeEnvironment* declarativeRecord;
};

// 9.1.1.5 Module Environment Records.
class ModuleEnvironment final : public DeclarativeEnvironment {
 public:
  explicit ModuleEnvironment(Environment* outer) : DeclarativeEnvironment(Kind::Module, outer) {}

  Result<Value> GetBindingValue(String* name, bool strict) override;
  Result<bool> DeleteBinding(String*) override { return false; }  // unreachable: module code is strict
  bool HasThisBinding() const override { return true; }
  // CreateImportBinding(N, M, N2): an immutable, initialized binding that reads N2 of M's environment.
  void CreateImportBinding(String* name, ModuleRecord* module, String* targetName);
};

// 9.1.2.1 GetIdentifierReference's environment walk is in the evaluator (it needs Reference Records).
// 9.4.3 GetThisEnvironment
Environment* GetThisEnvironment(Environment* env);

// 6.2.12 Private Names, 9.2 PrivateEnvironment Records.
class PrivateName final : public Cell {
 public:
  explicit PrivateName(String* description) : description(description) {}
  String* description;  // without the leading '#'
};

class PrivateEnvironment final : public Cell {
 public:
  explicit PrivateEnvironment(PrivateEnvironment* outer) : outer(outer) {}
  // ResolvePrivateIdentifier
  PrivateName* resolve(String* identifier) const;
  PrivateName* findOwn(String* identifier) const;

  void trace(Tracer& tracer) override;

  PrivateEnvironment* outer;
  std::vector<PrivateName*> names;
};

// The ModuleRecord's environment, for indirect bindings (module.cpp).
Environment* moduleEnvironmentOf(ModuleRecord* module);

}  // namespace snowman::interp
