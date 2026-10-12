// Symbol (20.4).
#include "builtins/util.h"
#include "runtime/function.h"
#include "runtime/wrappers.h"

namespace snowman::builtins {

namespace {

// 20.4.1.1 Symbol ( [ description ] )
Result<Value> Symbol_construct(NativeCall& call) {
  if (call.newTarget) return ThrowTypeError("Symbol is not a constructor");
  Value description = call.args[0];
  Value descString = description.isUndefined() ? Value::undefined() : Value(TRY(ToString(description)));
  return Value(make<Symbol>(descString));
}

// 20.4.2.2 Symbol.for ( key )
Result<Value> Symbol_for(NativeCall& call) {
  String* stringKey = TRY(ToString(call.args[0]));
  auto& registry = vm().symbolRegistry;
  for (auto& [key, symbol] : registry) {
    if (equals(key, stringKey)) return Value(symbol);
  }
  Symbol* newSymbol = make<Symbol>(Value(stringKey));
  registry.emplace_back(stringKey, newSymbol);
  return Value(newSymbol);
}

// 20.4.2.6 Symbol.keyFor ( sym )
Result<Value> Symbol_keyFor(NativeCall& call) {
  if (!call.args[0].isSymbol()) return ThrowTypeError("Symbol.keyFor: argument is not a symbol");
  for (auto& [key, symbol] : vm().symbolRegistry) {
    if (symbol == call.args[0].asSymbol()) return Value(key);
  }
  return Value::undefined();
}

// 20.4.3 ThisSymbolValue
Result<Symbol*> thisSymbolValue(Value value) {
  if (value.isSymbol()) return value.asSymbol();
  if (value.isObject()) {
    if (auto* s = value.asObject()->as<SymbolObject>()) return s->symbolData;
  }
  return ThrowTypeError("Symbol.prototype method called on incompatible receiver");
}

// 20.4.3.3.1 SymbolDescriptiveString
String* SymbolDescriptiveString(Symbol* sym) {
  Value desc = sym->description();
  std::u16string_view d = desc.isUndefined() ? u"" : desc.asString()->view();
  return concatStrings({u"Symbol(", d, u")"});
}

// 20.4.3.2 get Symbol.prototype.description
Result<Value> SymbolPrototype_description(NativeCall& call) {
  Symbol* sym = TRY(thisSymbolValue(call.thisValue));
  return sym->description();
}

// 20.4.3.3 Symbol.prototype.toString ( )
Result<Value> SymbolPrototype_toString(NativeCall& call) {
  Symbol* sym = TRY(thisSymbolValue(call.thisValue));
  return Value(SymbolDescriptiveString(sym));
}

// 20.4.3.4 Symbol.prototype.valueOf ( ), 20.4.3.5 Symbol.prototype [ @@toPrimitive ] ( hint )
Result<Value> SymbolPrototype_valueOf(NativeCall& call) { return Value(TRY(thisSymbolValue(call.thisValue))); }

void installSymbol(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor =
      defineConstructor(realm, Intrinsic::Symbol, Symbol_construct, 0, PropertyKey(u"Symbol"), proto, Intrinsic::SymbolPrototype);
  const WellKnownSymbols& s = vm().symbols;
#define SNOWMAN_DEFINE_WELL_KNOWN(name) defineValue(ctor, PropertyKey(u## #name), Value(s.name), Attr::None);
  SNOWMAN_WELL_KNOWN_SYMBOLS(SNOWMAN_DEFINE_WELL_KNOWN)
#undef SNOWMAN_DEFINE_WELL_KNOWN
  defineFunction(realm, ctor, PropertyKey(u"for"), 1, Symbol_for);
  defineFunction(realm, ctor, PropertyKey(u"keyFor"), 1, Symbol_keyFor);

  defineAccessor(realm, proto, PropertyKey(u"description"), SymbolPrototype_description, nullptr);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, SymbolPrototype_toString);
  defineFunction(realm, proto, PropertyKey(u"valueOf"), 0, SymbolPrototype_valueOf);
  defineFunction(realm, proto, PropertyKey(s.toPrimitive), 1, SymbolPrototype_valueOf, Attr::Configurable);
  defineValue(proto, PropertyKey(s.toStringTag), str(u"Symbol"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(Symbol, kOrderFundamental, installSymbol);

}  // namespace snowman::builtins
