// Proxy exotic objects (10.5), with every invariant check.
#include "runtime/proxy.h"

#include <algorithm>

#include "runtime/operations.h"
#include "runtime/string.h"
#include "runtime/vm.h"

namespace snowman {

namespace {

// The trap lookup shared by every internal method: throws on a revoked proxy, returns undefined or the
// callable trap.
Result<Value> getTrap(ProxyObject* proxy, const char16_t* name) {
  TRY(CheckStack());
  if (!proxy->proxyHandler) return ThrowTypeError("Cannot perform operation on a revoked proxy");
  return GetMethod(proxy->proxyHandler, PropertyKey(name));
}

}  // namespace

ProxyObject::ProxyObject(Object* target, Object* handler)
    : Object(nullptr),
      proxyTarget(target),
      proxyHandler(handler),
      callable_(target->isCallable()),
      constructor_(target->isConstructor()) {}

void ProxyObject::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(reinterpret_cast<Cell*>(proxyTarget));
  tracer.visit(reinterpret_cast<Cell*>(proxyHandler));
}

// 10.5.1 [[GetPrototypeOf]]
Result<Object*> ProxyObject::GetPrototypeOf() {
  Value trap = TRY(getTrap(this, u"getPrototypeOf"));
  Object* target = proxyTarget;
  if (trap.isUndefined()) return target->GetPrototypeOf();
  Value handlerProto = TRY(snowman::Call(trap, proxyHandler, {Value(target)}));
  if (!handlerProto.isObject() && !handlerProto.isNull())
    return ThrowTypeError("'getPrototypeOf' on proxy: trap returned neither object nor null");
  Object* proto = handlerProto.isNull() ? nullptr : handlerProto.asObject();
  bool extensibleTarget = TRY(snowman::IsExtensible(target));
  if (extensibleTarget) return proto;
  Object* targetProto = TRY(target->GetPrototypeOf());
  if (proto != targetProto)
    return ThrowTypeError("'getPrototypeOf' on proxy: proxy target is non-extensible but the trap did not return its actual prototype");
  return proto;
}

// 10.5.2 [[SetPrototypeOf]]
Result<bool> ProxyObject::SetPrototypeOf(Object* v) {
  Value trap = TRY(getTrap(this, u"setPrototypeOf"));
  Object* target = proxyTarget;
  if (trap.isUndefined()) return target->SetPrototypeOf(v);
  Value vv = v ? Value(v) : Value::null();
  bool booleanTrapResult = ToBoolean(TRY(snowman::Call(trap, proxyHandler, {Value(target), vv})));
  if (!booleanTrapResult) return false;
  bool extensibleTarget = TRY(snowman::IsExtensible(target));
  if (extensibleTarget) return true;
  Object* targetProto = TRY(target->GetPrototypeOf());
  if (v != targetProto)
    return ThrowTypeError("'setPrototypeOf' on proxy: trap returned truish for setting a new prototype on the non-extensible proxy target");
  return true;
}

// 10.5.3 [[IsExtensible]]
Result<bool> ProxyObject::IsExtensible() {
  Value trap = TRY(getTrap(this, u"isExtensible"));
  Object* target = proxyTarget;
  if (trap.isUndefined()) return snowman::IsExtensible(target);
  bool booleanTrapResult = ToBoolean(TRY(snowman::Call(trap, proxyHandler, {Value(target)})));
  bool targetResult = TRY(snowman::IsExtensible(target));
  if (booleanTrapResult != targetResult)
    return ThrowTypeError("'isExtensible' on proxy: trap result does not reflect extensibility of proxy target");
  return booleanTrapResult;
}

// 10.5.4 [[PreventExtensions]]
Result<bool> ProxyObject::PreventExtensions() {
  Value trap = TRY(getTrap(this, u"preventExtensions"));
  Object* target = proxyTarget;
  if (trap.isUndefined()) return target->PreventExtensions();
  bool booleanTrapResult = ToBoolean(TRY(snowman::Call(trap, proxyHandler, {Value(target)})));
  if (booleanTrapResult) {
    bool extensibleTarget = TRY(snowman::IsExtensible(target));
    if (extensibleTarget)
      return ThrowTypeError("'preventExtensions' on proxy: trap returned truish but the proxy target is extensible");
  }
  return booleanTrapResult;
}

// 10.5.5 [[GetOwnProperty]]
Result<std::optional<PropertyDescriptor>> ProxyObject::GetOwnProperty(const PropertyKey& key) {
  Value trap = TRY(getTrap(this, u"getOwnPropertyDescriptor"));
  Object* target = proxyTarget;
  if (trap.isUndefined()) return target->GetOwnProperty(key);
  Value trapResultObj = TRY(snowman::Call(trap, proxyHandler, {Value(target), key.toValue()}));
  if (!trapResultObj.isObject() && !trapResultObj.isUndefined())
    return ThrowTypeError("'getOwnPropertyDescriptor' on proxy: trap returned neither object nor undefined");
  auto targetDesc = TRY(target->GetOwnProperty(key));
  if (trapResultObj.isUndefined()) {
    if (!targetDesc) return std::optional<PropertyDescriptor>();
    if (!*targetDesc->configurable)
      return ThrowTypeError("'getOwnPropertyDescriptor' on proxy: trap returned undefined for a non-configurable property");
    bool extensibleTarget = TRY(snowman::IsExtensible(target));
    if (!extensibleTarget)
      return ThrowTypeError("'getOwnPropertyDescriptor' on proxy: trap returned undefined for a property of a non-extensible target");
    return std::optional<PropertyDescriptor>();
  }
  bool extensibleTarget = TRY(snowman::IsExtensible(target));
  PropertyDescriptor resultDesc = TRY(ToPropertyDescriptor(trapResultObj));
  CompletePropertyDescriptor(resultDesc);
  if (!IsCompatiblePropertyDescriptor(extensibleTarget, resultDesc, targetDesc))
    return ThrowTypeError("'getOwnPropertyDescriptor' on proxy: trap returned an incompatible descriptor");
  if (!*resultDesc.configurable) {
    if (!targetDesc || *targetDesc->configurable)
      return ThrowTypeError("'getOwnPropertyDescriptor' on proxy: trap reported non-configurability for a configurable or missing property");
    if (resultDesc.writable && !*resultDesc.writable && *targetDesc->writable)
      return ThrowTypeError("'getOwnPropertyDescriptor' on proxy: trap reported non-writability for a writable property");
  }
  return std::optional<PropertyDescriptor>(resultDesc);
}

// 10.5.6 [[DefineOwnProperty]]
Result<bool> ProxyObject::DefineOwnProperty(const PropertyKey& key, const PropertyDescriptor& desc) {
  Value trap = TRY(getTrap(this, u"defineProperty"));
  Object* target = proxyTarget;
  if (trap.isUndefined()) return target->DefineOwnProperty(key, desc);
  Value descObj = FromPropertyDescriptor(desc);
  bool booleanTrapResult = ToBoolean(TRY(snowman::Call(trap, proxyHandler, {Value(target), key.toValue(), descObj})));
  if (!booleanTrapResult) return false;
  auto targetDesc = TRY(target->GetOwnProperty(key));
  bool extensibleTarget = TRY(snowman::IsExtensible(target));
  bool settingConfigFalse = desc.configurable && !*desc.configurable;
  if (!targetDesc) {
    if (!extensibleTarget)
      return ThrowTypeError("'defineProperty' on proxy: trap returned truish for adding a property to a non-extensible target");
    if (settingConfigFalse)
      return ThrowTypeError("'defineProperty' on proxy: trap returned truish for defining a non-configurable property which does not exist on the target");
  } else {
    if (!IsCompatiblePropertyDescriptor(extensibleTarget, desc, targetDesc))
      return ThrowTypeError("'defineProperty' on proxy: trap returned truish for an incompatible descriptor");
    if (settingConfigFalse && *targetDesc->configurable)
      return ThrowTypeError("'defineProperty' on proxy: trap returned truish for defining a non-configurable property which is configurable on the target");
    if (targetDesc->isDataDescriptor() && !*targetDesc->configurable && *targetDesc->writable) {
      if (desc.writable && !*desc.writable)
        return ThrowTypeError("'defineProperty' on proxy: trap returned truish for defining a non-writable property which is writable on the target");
    }
  }
  return true;
}

// 10.5.7 [[HasProperty]]
Result<bool> ProxyObject::HasProperty(const PropertyKey& key) {
  Value trap = TRY(getTrap(this, u"has"));
  Object* target = proxyTarget;
  if (trap.isUndefined()) return target->HasProperty(key);
  bool booleanTrapResult = ToBoolean(TRY(snowman::Call(trap, proxyHandler, {Value(target), key.toValue()})));
  if (!booleanTrapResult) {
    auto targetDesc = TRY(target->GetOwnProperty(key));
    if (targetDesc) {
      if (!*targetDesc->configurable)
        return ThrowTypeError("'has' on proxy: trap returned falsish for a non-configurable property");
      bool extensibleTarget = TRY(snowman::IsExtensible(target));
      if (!extensibleTarget)
        return ThrowTypeError("'has' on proxy: trap returned falsish for a property of a non-extensible target");
    }
  }
  return booleanTrapResult;
}

// 10.5.8 [[Get]]
Result<Value> ProxyObject::Get(const PropertyKey& key, Value receiver) {
  Value trap = TRY(getTrap(this, u"get"));
  Object* target = proxyTarget;
  if (trap.isUndefined()) return target->Get(key, receiver);
  Value trapResult = TRY(snowman::Call(trap, proxyHandler, {Value(target), key.toValue(), receiver}));
  auto targetDesc = TRY(target->GetOwnProperty(key));
  if (targetDesc && !*targetDesc->configurable) {
    if (targetDesc->isDataDescriptor() && !*targetDesc->writable) {
      if (!SameValue(trapResult, *targetDesc->value))
        return ThrowTypeError("'get' on proxy: trap result differs from a non-writable, non-configurable data property");
    }
    if (targetDesc->isAccessorDescriptor() && targetDesc->get->isUndefined()) {
      if (!trapResult.isUndefined())
        return ThrowTypeError("'get' on proxy: trap returned a value for a non-configurable accessor without a getter");
    }
  }
  return trapResult;
}

// 10.5.9 [[Set]]
Result<bool> ProxyObject::Set(const PropertyKey& key, Value value, Value receiver) {
  Value trap = TRY(getTrap(this, u"set"));
  Object* target = proxyTarget;
  if (trap.isUndefined()) return target->Set(key, value, receiver);
  bool booleanTrapResult = ToBoolean(TRY(snowman::Call(trap, proxyHandler, {Value(target), key.toValue(), value, receiver})));
  if (!booleanTrapResult) return false;
  auto targetDesc = TRY(target->GetOwnProperty(key));
  if (targetDesc && !*targetDesc->configurable) {
    if (targetDesc->isDataDescriptor() && !*targetDesc->writable) {
      if (!SameValue(value, *targetDesc->value))
        return ThrowTypeError("'set' on proxy: trap returned truish for a non-writable, non-configurable property with a different value");
    }
    if (targetDesc->isAccessorDescriptor()) {
      if (targetDesc->set->isUndefined())
        return ThrowTypeError("'set' on proxy: trap returned truish for a non-configurable accessor without a setter");
    }
  }
  return true;
}

// 10.5.10 [[Delete]]
Result<bool> ProxyObject::Delete(const PropertyKey& key) {
  Value trap = TRY(getTrap(this, u"deleteProperty"));
  Object* target = proxyTarget;
  if (trap.isUndefined()) return target->Delete(key);
  bool booleanTrapResult = ToBoolean(TRY(snowman::Call(trap, proxyHandler, {Value(target), key.toValue()})));
  if (!booleanTrapResult) return false;
  auto targetDesc = TRY(target->GetOwnProperty(key));
  if (!targetDesc) return true;
  if (!*targetDesc->configurable)
    return ThrowTypeError("'deleteProperty' on proxy: trap returned truish for a non-configurable property");
  bool extensibleTarget = TRY(snowman::IsExtensible(target));
  if (!extensibleTarget)
    return ThrowTypeError("'deleteProperty' on proxy: trap returned truish for a property of a non-extensible target");
  return true;
}

// 10.5.11 [[OwnPropertyKeys]]
Result<std::vector<PropertyKey>> ProxyObject::OwnPropertyKeys() {
  Value trap = TRY(getTrap(this, u"ownKeys"));
  Object* target = proxyTarget;
  if (trap.isUndefined()) return target->OwnPropertyKeys();
  Value trapResultArray = TRY(snowman::Call(trap, proxyHandler, {Value(target)}));
  ValueVector trapResultValues = TRY(CreateListFromArrayLike(trapResultArray, true));
  std::vector<PropertyKey> trapResult;
  for (Value v : trapResultValues) {
    PropertyKey k = v.isSymbol() ? PropertyKey(v.asSymbol()) : PropertyKey(v.asString());
    if (std::find(trapResult.begin(), trapResult.end(), k) != trapResult.end())
      return ThrowTypeError("'ownKeys' on proxy: trap returned duplicate entries");
    trapResult.push_back(k);
  }
  bool extensibleTarget = TRY(snowman::IsExtensible(target));
  std::vector<PropertyKey> targetKeys = TRY(target->OwnPropertyKeys());
  RootedKeys rootedTargetKeys(targetKeys);
  std::vector<PropertyKey> targetConfigurableKeys, targetNonconfigurableKeys;
  for (const PropertyKey& key : targetKeys) {
    auto desc = TRY(target->GetOwnProperty(key));
    if (desc && !*desc->configurable) targetNonconfigurableKeys.push_back(key);
    else targetConfigurableKeys.push_back(key);
  }
  if (extensibleTarget && targetNonconfigurableKeys.empty()) return trapResult;
  std::vector<PropertyKey> uncheckedResultKeys = trapResult;
  auto removeKey = [&](const PropertyKey& key) {
    auto it = std::find(uncheckedResultKeys.begin(), uncheckedResultKeys.end(), key);
    if (it == uncheckedResultKeys.end()) return false;
    uncheckedResultKeys.erase(it);
    return true;
  };
  for (const PropertyKey& key : targetNonconfigurableKeys) {
    if (!removeKey(key)) return ThrowTypeError("'ownKeys' on proxy: trap result did not include a non-configurable key");
  }
  if (extensibleTarget) return trapResult;
  for (const PropertyKey& key : targetConfigurableKeys) {
    if (!removeKey(key))
      return ThrowTypeError("'ownKeys' on proxy: trap result did not include every key of the non-extensible target");
  }
  if (!uncheckedResultKeys.empty())
    return ThrowTypeError("'ownKeys' on proxy: trap returned extra keys but proxy target is non-extensible");
  return trapResult;
}

// 10.5.12 [[Call]]
Result<Value> ProxyObject::Call(Value thisArgument, Args args) {
  Value trap = TRY(getTrap(this, u"apply"));
  Object* target = proxyTarget;
  if (trap.isUndefined()) return snowman::Call(target, thisArgument, args);
  Object* argArray = CreateArrayFromList(args);
  return snowman::Call(trap, proxyHandler, {Value(target), thisArgument, Value(argArray)});
}

// 10.5.13 [[Construct]]
Result<Object*> ProxyObject::Construct(Args args, Object* newTarget) {
  Value trap = TRY(getTrap(this, u"construct"));
  Object* target = proxyTarget;
  if (trap.isUndefined()) return snowman::Construct(target, args, newTarget);
  Object* argArray = CreateArrayFromList(args);
  Value newObj = TRY(snowman::Call(trap, proxyHandler, {Value(target), Value(argArray), Value(newTarget)}));
  if (!newObj.isObject()) return ThrowTypeError("'construct' on proxy: trap returned non-object");
  return newObj.asObject();
}

// 10.5.14 ProxyCreate
Result<ProxyObject*> ProxyCreate(Value target, Value handler) {
  if (!target.isObject()) return ThrowTypeError("Cannot create proxy with a non-object as target");
  if (!handler.isObject()) return ThrowTypeError("Cannot create proxy with a non-object as handler");
  return make<ProxyObject>(target.asObject(), handler.asObject());
}

}  // namespace snowman
