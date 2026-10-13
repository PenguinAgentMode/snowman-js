// Realms: CreateRealm and the built-in installer registry.
#include "runtime/realm.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "runtime/builtins.h"
#include "runtime/function.h"
#include "runtime/operations.h"
#include "runtime/property_map.h"
#include "runtime/string.h"
#include "runtime/vm.h"

namespace snowman {

namespace {

struct Registration {
  const char* name;
  int order;
  Installer installer;
};

std::vector<Registration>& registry() {
  static auto* list = new std::vector<Registration>();
  return *list;
}

// %Object.prototype% is an immutable prototype exotic object (10.4.7).
class ImmutablePrototypeObject : public Object {
 public:
  using Object::Object;
  bool hasOrdinaryPropertyStorage() const override { return true; }
  // 10.4.7.2 SetImmutablePrototype
  Result<bool> SetPrototypeOf(Object* prototype) override {
    Object* current = TRY(GetPrototypeOf());
    return prototype == current;
  }
};

Result<Value> functionPrototypeCall(NativeCall&) { return Value::undefined(); }

}  // namespace

Realm::Realm() = default;

void Realm::trace(Tracer& tracer) {
  for (Object* object : intrinsics_) tracer.visit(reinterpret_cast<Cell*>(object));
  tracer.visit(reinterpret_cast<Cell*>(globalObject_));
  tracer.visit(globalEnv);
  tracer.visit(hostDefined);
}

void registerBuiltin(const char* name, int order, Installer installer) {
  registry().push_back({name, order, installer});
}

Realm* CreateRealm() {
  VM& v = vm();
  Realm* realm = make<Realm>();
  v.realms.push_back(realm);
  if (!v.initialRealm) v.initialRealm = realm;

  Object* objectPrototype = make<ImmutablePrototypeObject>(nullptr);
  realm->setIntrinsic(Intrinsic::ObjectPrototype, objectPrototype);
  BuiltinFunction* functionPrototype = make<BuiltinFunction>(realm, functionPrototypeCall, objectPrototype, false);
  realm->setIntrinsic(Intrinsic::FunctionPrototype, functionPrototype);
  functionPrototype->properties().put(PropertyKey(u"length"), PropertySlot::data(Value::number(0), Attr::Configurable));
  functionPrototype->properties().put(PropertyKey(u"name"), PropertySlot::data(emptyString(), Attr::Configurable));
  functionPrototype->initialName = emptyString();
  realm->setGlobalObject(OrdinaryObjectCreate(objectPrototype));

  std::vector<Registration> installers = registry();
  std::stable_sort(installers.begin(), installers.end(), [](const Registration& a, const Registration& b) {
    if (a.order != b.order) return a.order < b.order;
    return std::strcmp(a.name, b.name) < 0;
  });
  ExecutionContext context;
  context.realm = realm;
  v.pushContext(&context);
  for (const Registration& r : installers) r.installer(*realm);
  v.popContext();
  return realm;
}

}  // namespace snowman
