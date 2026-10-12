// The object model: the essential internal methods as virtuals (ordinary behaviour in the base class),
// plus [[Call]]/[[Construct]]. INTERFACES.md section 5.
#pragma once

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

#include "runtime/property.h"
#include "runtime/result.h"
#include "runtime/value.h"

namespace snowman {

class PropertyMap;  // ordinary own-property storage, defined by the runtime (property_map.h)

// Args: a read-only view of an argument list. args[i] is undefined past the end. Views built from an
// initializer list are valid for the full-expression only: `Call(f, thisValue, {a, b})`.
class Args {
 public:
  Args() = default;
  Args(const Value* values, size_t count) : data_(values), size_(count) {}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winit-list-lifetime"
  Args(std::initializer_list<Value> values) : data_(values.begin()), size_(values.size()) {}
#pragma GCC diagnostic pop
  Args(const ValueVector& values) : data_(values.data()), size_(values.size()) {}

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  Value operator[](size_t i) const { return i < size_ ? data_[i] : Value::undefined(); }
  const Value* begin() const { return data_; }
  const Value* end() const { return data_ + size_; }
  Args slice(size_t from) const { return from >= size_ ? Args() : Args(data_ + from, size_ - from); }

 private:
  const Value* data_ = nullptr;
  size_t size_ = 0;
};

class Object : public Cell {
 public:
  // An ordinary, extensible object. prototype may be nullptr (null).
  explicit Object(Object* prototype);
  ~Object() override;

  // The essential internal methods (10.1). The base class is the ordinary object; exotic objects override.
  // Prototypes are Object* with nullptr for null.
  virtual Result<Object*> GetPrototypeOf();
  virtual Result<bool> SetPrototypeOf(Object* prototype);
  virtual Result<bool> IsExtensible();
  virtual Result<bool> PreventExtensions();
  virtual Result<std::optional<PropertyDescriptor>> GetOwnProperty(const PropertyKey& key);
  virtual Result<bool> DefineOwnProperty(const PropertyKey& key, const PropertyDescriptor& desc);
  virtual Result<bool> HasProperty(const PropertyKey& key);
  virtual Result<Value> Get(const PropertyKey& key, Value receiver);
  virtual Result<bool> Set(const PropertyKey& key, Value value, Value receiver);
  virtual Result<bool> Delete(const PropertyKey& key);
  virtual Result<std::vector<PropertyKey>> OwnPropertyKeys();

  // [[Call]] and [[Construct]] exist iff isCallable() / isConstructor(). The defaults throw a TypeError.
  virtual bool isCallable() const { return false; }
  virtual bool isConstructor() const { return false; }
  virtual Result<Value> Call(Value thisArgument, Args args);
  virtual Result<Object*> Construct(Args args, Object* newTarget);

  // False for objects whose [[GetPrototypeOf]] is not the ordinary one (Proxy): OrdinarySetPrototypeOf's
  // cycle check stops there.
  virtual bool hasOrdinaryGetPrototypeOf() const { return true; }

  // Internal slots live in subclasses; test and downcast with these.
  template <class T>
  bool is() const {
    return dynamic_cast<const T*>(this) != nullptr;
  }
  template <class T>
  T* as() {
    return dynamic_cast<T*>(this);
  }

  // Direct, unobservable access for setting up objects and for exotic objects that fall back to the
  // ordinary behaviour (OrdinaryGetOwnProperty etc. are the base-class methods, called qualified).
  Object* prototype() const { return prototype_; }
  void setPrototypeDirect(Object* prototype) { prototype_ = prototype; }
  bool extensible() const { return extensible_; }
  PropertyMap& properties() { return *properties_; }

  // [[PrivateElements]]: an interp-owned cell (src/interp/), nullptr when the object has none. Traced. Any
  // object can carry private elements (the class-field return-override trick reaches proxies, arrays, ...).
  Cell* privateElements = nullptr;

  void trace(Tracer& tracer) override;

 protected:
  Object* prototype_;
  bool extensible_ = true;
  std::unique_ptr<PropertyMap> properties_;
};

// An object with a [[ParameterMap]] slot: an arguments exotic object or an unmapped arguments object (10.4.4).
// The interpreter creates and subclasses them; Object.prototype.toString tests obj->is<ArgumentsObject>().
class ArgumentsObject : public Object {
 public:
  using Object::Object;
};

}  // namespace snowman
