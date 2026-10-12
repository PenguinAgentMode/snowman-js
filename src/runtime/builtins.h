// Declaring built-ins: one file per built-in in src/builtins/, each registering an installer that
// CreateRealm runs. INTERFACES.md section 9.
#pragma once

#include <string_view>

#include "runtime/function.h"
#include "runtime/object.h"
#include "runtime/realm.h"

namespace snowman {

// Installer order. CreateRealm first makes %Object.prototype%, %Function.prototype% and the global object,
// then runs installers by order, then name. Use intrinsics only from earlier orders (or your own file).
constexpr int kOrderFundamental = 10;  // Object, Function, Error types, Symbol, %IteratorPrototype%
constexpr int kOrderDefault = 20;      // everything else
constexpr int kOrderLate = 30;         // things that read other built-ins (Annex B, host bindings)

using Installer = void (*)(Realm& realm);
void registerBuiltin(const char* name, int order, Installer installer);

#define SNOWMAN_BUILTIN(name, order, installer) \
  [[maybe_unused]] static const bool snowman_builtin_##name = (::snowman::registerBuiltin(#name, order, installer), true)

// CreateBuiltinFunction (10.3.4): a function object with "length" and "name" set. prototype defaults to
// %Function.prototype%; prefix is "get", "set" or "bound" etc. Symbol names become "[description]".
BuiltinFunction* createBuiltinFunction(Realm& realm, NativeFn fn, int length, const PropertyKey& name,
                                       bool isConstructor = false, Object* prototype = nullptr,
                                       std::u16string_view prefix = {});

// A method: writable, non-enumerable, configurable unless attrs says otherwise.
BuiltinFunction* defineFunction(Realm& realm, Object* target, const PropertyKey& name, int length, NativeFn fn,
                                Attr attrs = Attr::WritableConfigurable);
// An accessor property; getter/setter may be nullptr (undefined). Non-enumerable, configurable by default.
void defineAccessor(Realm& realm, Object* target, const PropertyKey& name, NativeFn getter, NativeFn setter,
                    Attr attrs = Attr::Configurable);
// A data property, writable/non-enumerable/configurable by default.
void defineValue(Object* target, const PropertyKey& name, Value value, Attr attrs = Attr::WritableConfigurable);
// A global binding on the realm's global object (writable, non-enumerable, configurable).
void defineGlobal(Realm& realm, const PropertyKey& name, Value value);

// A constructor/prototype pair: creates the constructor function, links C.prototype (non-writable,
// non-enumerable, non-configurable) and P.constructor (writable, configurable), records both intrinsics and
// defines the global binding (unless defineGlobalBinding is false).
BuiltinFunction* defineConstructor(Realm& realm, Intrinsic constructorId, NativeFn fn, int length,
                                   const PropertyKey& name, Object* prototypeObject, Intrinsic prototypeId,
                                   bool defineGlobalBinding = true);

}  // namespace snowman
