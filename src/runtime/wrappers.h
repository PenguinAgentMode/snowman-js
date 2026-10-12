// Objects with a primitive-value internal slot: Boolean, Number, String (exotic, 10.4.3), Symbol and
// BigInt wrapper objects.
#pragma once

#include "runtime/object.h"

namespace snowman {

class BigInt;
class String;
class Symbol;

class BooleanObject : public Object {
 public:
  BooleanObject(Object* prototype, bool value) : Object(prototype), booleanData(value) {}
  bool booleanData;
};

class NumberObject : public Object {
 public:
  NumberObject(Object* prototype, double value) : Object(prototype), numberData(value) {}
  double numberData;
};

class SymbolObject : public Object {
 public:
  SymbolObject(Object* prototype, Symbol* value) : Object(prototype), symbolData(value) {}
  Symbol* symbolData;
  void trace(Tracer& tracer) override;
};

class BigIntObject : public Object {
 public:
  BigIntObject(Object* prototype, BigInt* value) : Object(prototype), bigintData(value) {}
  BigInt* bigintData;
  void trace(Tracer& tracer) override;
};

// A String exotic object: integer-indexed own properties for its code units, plus "length".
class StringObject : public Object {
 public:
  // StringCreate (10.4.3.4): defines "length".
  StringObject(Object* prototype, String* value);
  String* stringData;

  Result<std::optional<PropertyDescriptor>> GetOwnProperty(const PropertyKey& key) override;
  Result<bool> DefineOwnProperty(const PropertyKey& key, const PropertyDescriptor& desc) override;
  Result<std::vector<PropertyKey>> OwnPropertyKeys() override;
  void trace(Tracer& tracer) override;

  // 10.4.3.5 StringGetOwnProperty
  std::optional<PropertyDescriptor> StringGetOwnProperty(const PropertyKey& key) const;
};

}  // namespace snowman
