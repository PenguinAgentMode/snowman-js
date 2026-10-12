// Proxy exotic objects (10.5).
#pragma once

#include "runtime/object.h"

namespace snowman {

class ProxyObject : public Object {
 public:
  ProxyObject(Object* target, Object* handler);

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
  bool isCallable() const override { return callable_; }
  bool isConstructor() const override { return constructor_; }
  Result<Value> Call(Value thisArgument, Args args) override;
  Result<Object*> Construct(Args args, Object* newTarget) override;
  bool hasOrdinaryGetPrototypeOf() const override { return false; }
  void trace(Tracer& tracer) override;

  // [[ProxyTarget]] / [[ProxyHandler]]; both nullptr once revoked.
  Object* proxyTarget;
  Object* proxyHandler;
  void revoke() {
    proxyTarget = nullptr;
    proxyHandler = nullptr;
  }

 private:
  bool callable_;
  bool constructor_;
};

// 10.5.14 ProxyCreate
Result<ProxyObject*> ProxyCreate(Value target, Value handler);

}  // namespace snowman
