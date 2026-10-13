// The Intl object (ECMA-402 8): Intl.getCanonicalLocales, Intl.supportedValuesOf and the constructors.
#include "builtins/intl/intl.h"
#include "builtins/intl/services.h"
#include "builtins/temporal/time_zone.h"
#include "runtime/symbol.h"

namespace snowman::intl {

namespace {

// 8.3.1 Intl.getCanonicalLocales ( locales )
Result<Value> Intl_getCanonicalLocales(NativeCall& call) {
  std::vector<std::string> ll = TRY(CanonicalizeLocaleList(call.args[0]));
  ValueVector list;
  for (const std::string& l : ll) list.push_back(jsString(l));
  return Value(CreateArrayFromList(list));
}

// 8.3.2 Intl.supportedValuesOf ( key )
Result<Value> Intl_supportedValuesOf(NativeCall& call) {
  String* key = TRY(ToString(call.args[0]));
  std::u16string_view k = key->view();
  std::vector<std::string> list;
  if (k == u"calendar") list = data::calendars();
  else if (k == u"collation") list = data::collationTypes();
  else if (k == u"currency") list = data::currencies();
  else if (k == u"numberingSystem") list = data::numberingSystems();
  else if (k == u"timeZone") {
    // The primary identifiers of Temporal's time zone data, which DateTimeFormat and Temporal accept.
    ValueVector zones;
    for (const std::u16string& z : temporal::AvailablePrimaryTimeZoneIdentifiers()) zones.push_back(Value(newString(z)));
    return Value(CreateArrayFromList(zones));
  }
  else if (k == u"unit") list = data::units();
  else return ThrowRangeError("Invalid key : " + key->toUtf8());
  ValueVector values;
  for (const std::string& v : list) values.push_back(jsString(v));
  return Value(CreateArrayFromList(values));
}

void installIntl(Realm& realm) {
  IntlObject* intl = make<IntlObject>(realm.intrinsic(Intrinsic::ObjectPrototype));
  intl->fallbackSymbol = make<Symbol>(Value(newString(std::u16string_view(u"IntlLegacyConstructedSymbol"))));
  realm.setIntrinsic(Intrinsic::Intl, intl);
  defineGlobal(realm, PropertyKey(u"Intl"), intl);
  defineValue(intl, PropertyKey(vm().symbols.toStringTag), str(u"Intl"), Attr::Configurable);
  defineFunction(realm, intl, PropertyKey(u"getCanonicalLocales"), 1, Intl_getCanonicalLocales);
  defineFunction(realm, intl, PropertyKey(u"supportedValuesOf"), 1, Intl_supportedValuesOf);
  installCollator(realm, intl);
  installDateTimeFormat(realm, intl);
  installDisplayNames(realm, intl);
  installDurationFormat(realm, intl);
  installListFormat(realm, intl);
  installLocale(realm, intl);
  installNumberFormat(realm, intl);
  installPluralRules(realm, intl);
  installRelativeTimeFormat(realm, intl);
  installSegmenter(realm, intl);
}

}  // namespace

BuiltinFunction* defineIntlConstructor(Realm& realm, Object* intl, Intrinsic constructorId, NativeFn fn, int length,
                                       std::u16string_view name, Intrinsic prototypeId, Object** prototypeOut) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor = defineConstructor(realm, constructorId, fn, length, PropertyKey(name), proto, prototypeId, false);
  defineValue(intl, PropertyKey(name), ctor);
  std::u16string tag = u"Intl.";
  tag += name;
  defineValue(proto, PropertyKey(vm().symbols.toStringTag), Value(newString(tag)), Attr::Configurable);
  if (prototypeOut) *prototypeOut = proto;
  return ctor;
}

void IntlObject::trace(Tracer& tracer) {
  Object::trace(tracer);
  tracer.visit(reinterpret_cast<Cell*>(fallbackSymbol));
}

Symbol* fallbackSymbol() { return builtins::intrinsic(Intrinsic::Intl)->as<IntlObject>()->fallbackSymbol; }

std::span<const std::string> commonLocales() { return data::locales(); }

SNOWMAN_BUILTIN(Intl, kOrderDefault, installIntl);

}  // namespace snowman::intl
