// Environment Records (ECMA-262 9.1) and PrivateEnvironment Records (9.2). Binding names are atoms, so names
// compare by pointer.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <type_traits>
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
    // A module import binding: a binding of another module's environment (ModuleEnvironment::imports_).
    bool indirect = false;
  };

  // The bindings, the first few stored in the record itself (most records hold a handful, and a function
  // call creates one). Bindings are trivially copyable.
  class BindingList {
   public:
    static constexpr uint32_t kInline = 4;
    BindingList() = default;
    BindingList(const BindingList&) = delete;
    BindingList& operator=(const BindingList&) = delete;
    ~BindingList() {
      if (data_ != inline_) ::operator delete(data_);
    }
    uint32_t size() const { return size_; }
    Binding* data() { return data_; }
    const Binding* data() const { return data_; }
    Binding& operator[](size_t i) { return data_[i]; }
    Binding* begin() { return data_; }
    Binding* end() { return data_ + size_; }
    const Binding* begin() const { return data_; }
    const Binding* end() const { return data_ + size_; }
    Binding& back() { return data_[size_ - 1]; }
    void reserve(size_t count) {
      if (count > capacity_) grow(count);
    }
    void push_back(const Binding& binding) {
      if (size_ == capacity_) grow(size_t(capacity_) * 2);
      data_[size_++] = binding;
    }
    void assign(const Binding* bindings, size_t count) {
      size_ = 0;
      reserve(count);
      if (count) std::memcpy(static_cast<void*>(data_), bindings, count * sizeof(Binding));
      size_ = uint32_t(count);
    }
    void erase(size_t at) {
      std::memmove(static_cast<void*>(data_ + at), data_ + at + 1, (size_ - at - 1) * sizeof(Binding));
      size_--;
    }

   private:
    void grow(size_t capacity) {
      auto* grown = static_cast<Binding*>(::operator new(capacity * sizeof(Binding)));
      std::memcpy(static_cast<void*>(grown), data_, size_ * sizeof(Binding));
      if (data_ != inline_) ::operator delete(data_);
      data_ = grown;
      capacity_ = uint32_t(capacity);
    }

    Binding* data_ = inline_;
    uint32_t size_ = 0;
    uint32_t capacity_ = kInline;
    Binding inline_[kInline];
  };
  static_assert(std::is_trivially_copyable_v<Binding>);

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
  // Binding slots: a binding keeps its slot until a binding is deleted (the record is dynamic by then).
  Binding* bindingAt(uint32_t slot) { return slot < bindings_.size() ? &bindings_[slot] : nullptr; }
  uint32_t slotOf(const Binding* binding) const { return uint32_t(binding - bindings_.data()); }
  // SetMutableBinding on an existing binding.
  Result<void> setBinding(Binding& binding, String* name, Value value, bool strict);
  // A record that gained a binding after its instantiation (an eval-declared var) or lost one: its bindings
  // are no longer those of every record its code creates, so the identifier caches don't look through it.
  bool isDynamic() const { return dynamic_; }
  // A function record's whole binding list at once (FunctionDeclarationInstantiation's fast path).
  void assignBindings(const std::vector<Binding>& bindings) {
    bindings_.assign(bindings.data(), bindings.size());
    index_.reset();
  }
  // A new binding initialized to `value` (CreateMutableBinding + InitializeBinding).
  void createInitializedBinding(String* name, Value value, bool mutable_ = true);
  const BindingList& bindings() const { return bindings_; }
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

  BindingList bindings_;
  std::unique_ptr<std::unordered_map<String*, uint32_t>> index_;  // built by find() once a record has many bindings
  bool dynamic_ = false;
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
  ObjectEnvironment(Object* bindingObject, bool isWithEnvironment, Environment* outer);

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

 private:
  bool direct_;  // ownDataSlot applies: an ordinary binding object, not a with environment
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

  void trace(Tracer& tracer) override;

 private:
  // The import bindings' targets: binding `name` is `targetName` of `module`'s environment.
  struct Import {
    String* name;
    ModuleRecord* module;
    String* targetName;
  };
  std::vector<Import> imports_;
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
