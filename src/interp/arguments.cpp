// Arguments exotic objects (10.4.4) and unmapped arguments objects (10.4.4.6). The [[ParameterMap]] of a
// mapped arguments object is unobservable, so it is a list of parameter names (index → name) that reads and
// writes the function's environment directly.
#include "interp/interpreter.h"
#include "runtime/builtins.h"
#include "runtime/operations.h"
#include "runtime/vm.h"

namespace snowman::interp {

namespace {

class MappedArgumentsObject final : public ArgumentsObject {
 public:
  MappedArgumentsObject(Object* prototype, Environment* env) : ArgumentsObject(prototype), env_(env) {}

  void map(uint32_t index, String* name) {
    if (mapped_.size() <= index) mapped_.resize(index + 1, nullptr);
    mapped_[index] = name;
  }

  Result<std::optional<PropertyDescriptor>> GetOwnProperty(const PropertyKey& key) override {
    auto desc = TRY(Object::GetOwnProperty(key));
    if (!desc) return desc;
    if (String* name = mappedName(key)) desc->value = TRY(env_->GetBindingValue(name, false));
    return desc;
  }

  Result<bool> DefineOwnProperty(const PropertyKey& key, const PropertyDescriptor& desc) override {
    String* name = mappedName(key);
    PropertyDescriptor newArgDesc = desc;
    if (name && desc.isDataDescriptor() && !desc.value && desc.writable && !*desc.writable) {
      newArgDesc.value = TRY(env_->GetBindingValue(name, false));
    }
    bool allowed = TRY(Object::DefineOwnProperty(key, newArgDesc));
    if (!allowed) return false;
    if (name) {
      if (desc.isAccessorDescriptor()) {
        unmap(key);
      } else {
        if (desc.value) TRY(env_->SetMutableBinding(name, *desc.value, false));
        if (desc.writable && !*desc.writable) unmap(key);
      }
    }
    return true;
  }

  Result<Value> Get(const PropertyKey& key, Value receiver) override {
    String* name = mappedName(key);
    if (!name) return Object::Get(key, receiver);
    return env_->GetBindingValue(name, false);
  }

  Result<bool> Set(const PropertyKey& key, Value value, Value receiver) override {
    bool isMapped = receiver.isObject() && receiver.asObject() == this && mappedName(key);
    if (isMapped) TRY(env_->SetMutableBinding(mappedName(key), value, false));
    return Object::Set(key, value, receiver);
  }

  Result<bool> Delete(const PropertyKey& key) override {
    String* name = mappedName(key);
    bool result = TRY(Object::Delete(key));
    if (result && name) unmap(key);
    return result;
  }

  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(env_);
    for (String* name : mapped_) tracer.visit(reinterpret_cast<Cell*>(name));
  }

 private:
  String* mappedName(const PropertyKey& key) const {
    if (!key.isIndex() || key.asIndex() >= mapped_.size()) return nullptr;
    return mapped_[key.asIndex()];
  }
  void unmap(const PropertyKey& key) { mapped_[key.asIndex()] = nullptr; }

  Environment* env_;
  std::vector<String*> mapped_;
};

void defineIndexedArguments(Object* obj, Args args) {
  for (uint32_t index = 0; index < args.size(); index++) {
    defineValue(obj, PropertyKey::index(index), args[index], Attr::All);
  }
}

}  // namespace

// 10.4.4.6 CreateUnmappedArgumentsObject
Object* CreateUnmappedArgumentsObject(Args args) {
  Realm* realm = vm().currentRealm();
  auto* obj = snowman::make<ArgumentsObject>(realm->intrinsic(Intrinsic::ObjectPrototype));
  defineValue(obj, PropertyKey(u"length"), Value::number(double(args.size())), Attr::WritableConfigurable);
  defineIndexedArguments(obj, args);
  defineValue(obj, PropertyKey(vm().symbols.iterator), Value(realm->intrinsic(Intrinsic::ArrayPrototypeValues)),
              Attr::WritableConfigurable);
  Value thrower(realm->intrinsic(Intrinsic::ThrowTypeError));
  (void)obj->DefineOwnProperty(PropertyKey(u"callee"), PropertyDescriptor::accessor(thrower, thrower, Attr::None));
  return obj;
}

// 10.4.4.7 CreateMappedArgumentsObject
Object* CreateMappedArgumentsObject(ECMAScriptFunction* func, const std::vector<std::u16string>& formals, Args args,
                                    Environment* env) {
  Realm* realm = vm().currentRealm();
  auto* obj = snowman::make<MappedArgumentsObject>(realm->intrinsic(Intrinsic::ObjectPrototype), env);
  defineIndexedArguments(obj, args);
  defineValue(obj, PropertyKey(u"length"), Value::number(double(args.size())), Attr::WritableConfigurable);
  std::vector<String*> mappedNames;
  for (size_t i = formals.size(); i-- > 0;) {
    String* name = atomFor(formals[i]);
    bool seen = false;
    for (String* n : mappedNames) seen = seen || n == name;
    if (seen) continue;
    mappedNames.push_back(name);
    if (i < args.size()) obj->map(uint32_t(i), name);
  }
  defineValue(obj, PropertyKey(vm().symbols.iterator), Value(realm->intrinsic(Intrinsic::ArrayPrototypeValues)),
              Attr::WritableConfigurable);
  defineValue(obj, PropertyKey(u"callee"), Value(func), Attr::WritableConfigurable);
  return obj;
}

}  // namespace snowman::interp
