// A small driver for the runtime core, built outside ./engine:
//   g++ -std=c++20 -O1 -g -Isrc tools/runtime/core_test.cpp $(ls src/runtime/*.cpp src/builtins/*.cpp src/unicode/*.cpp) -o /tmp/core_test
#include <cmath>
#include <cstdio>
#include <string>

#include "runtime/array.h"
#include "runtime/bigint.h"
#include "runtime/builtins.h"
#include "runtime/operations.h"
#include "runtime/property_map.h"
#include "runtime/string.h"
#include "runtime/vm.h"

using namespace snowman;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                       \
    }                                                                   \
  } while (0)

static std::string str(String* s) { return s->toUtf8(); }
static std::string num(double d, int radix = 10) { return str(NumberToString(d, radix)); }

int main() {
  Realm* realm = CreateRealm();
  vm().initialRealm = realm;

  CHECK(num(0) == "0");
  CHECK(num(-0.0) == "0");
  CHECK(num(1.5) == "1.5");
  CHECK(num(-1.5) == "-1.5");
  CHECK(num(1e21) == "1e+21");
  CHECK(num(1e20) == "100000000000000000000");
  CHECK(num(123456789012345680000.0) == "123456789012345680000");
  CHECK(num(0.000001) == "0.000001");
  CHECK(num(1e-7) == "1e-7");
  CHECK(num(5e-324) == "5e-324");
  CHECK(num(1.7976931348623157e308) == "1.7976931348623157e+308");
  CHECK(num(0.1 + 0.2) == "0.30000000000000004");
  CHECK(num(255, 16) == "ff");
  CHECK(num(0.5, 2) == "0.1");
  CHECK(num(-255, 2) == "-11111111");
  CHECK(num(NAN) == "NaN");
  CHECK(num(-INFINITY) == "-Infinity");

  CHECK(StringToNumber(u"  12  ") == 12);
  CHECK(StringToNumber(u"") == 0);
  CHECK(StringToNumber(u"0x1F") == 31);
  CHECK(std::isnan(StringToNumber(u"-0x1F")));
  CHECK(StringToNumber(u"1e3") == 1000);
  CHECK(StringToNumber(u".5") == 0.5);
  CHECK(StringToNumber(u"5.") == 5);
  CHECK(std::isnan(StringToNumber(u".")));
  CHECK(std::isnan(StringToNumber(u"1e")));
  CHECK(StringToNumber(u"-Infinity") == -INFINITY);
  CHECK(std::isnan(StringToNumber(u"infinity")));
  CHECK(std::signbit(StringToNumber(u"-0")));
  CHECK(StringToNumber(u"\u00A0\uFEFF42\u2028") == 42);
  CHECK(StringToNumber(u"0b101") == 5);
  CHECK(StringToNumber(u"0o17") == 15);

  CHECK(DoubleToInt32(4294967296.0 + 5) == 5);
  CHECK(DoubleToInt32(2147483648.0) == -2147483648);
  CHECK(DoubleToInt32(-1) == -1);
  CHECK(DoubleToUint32(-1) == 4294967295u);

  // BigInt
  BigInt* a = *BigInt::fromString(u"123456789012345678901234567890");
  BigInt* b = *BigInt::fromString(u"-987654321");
  CHECK(BigInt::multiply(a, b)->toString() == u"-121932631124828532112482853211126352690");
  CHECK(BigInt::divide(a, b)->toString() == u"-124999998873437499901");
  CHECK(BigInt::remainder(a, b)->toString() == u"574845669");
  CHECK(BigInt::add(a, b)->toString() == u"123456789012345678900246913569");
  CHECK(BigInt::fromInt64(-5)->toString(2) == u"-101");
  CHECK(BigInt::bitwiseAnd(BigInt::fromInt64(-5), BigInt::fromInt64(7))->toString() == u"3");
  CHECK(BigInt::bitwiseOr(BigInt::fromInt64(-5), BigInt::fromInt64(2))->toString() == u"-5");
  CHECK(BigInt::bitwiseXor(BigInt::fromInt64(-5), BigInt::fromInt64(-3))->toString() == u"6");
  CHECK(BigInt::signedRightShift(BigInt::fromInt64(-5), BigInt::fromInt64(1))->toString() == u"-3");
  CHECK(BigInt::leftShift(BigInt::fromInt64(3), BigInt::fromInt64(100))->toString() == u"3802951800684688204490109616128");
  CHECK(BigInt::asIntN(8, BigInt::fromInt64(255))->toString() == u"-1");
  CHECK(BigInt::asUintN(8, BigInt::fromInt64(-1))->toString() == u"255");
  CHECK(BigInt::asIntN(64, BigInt::fromUint64(0x8000000000000000ull))->toString() == u"-9223372036854775808");
  CHECK(BigInt::exponentiate(BigInt::fromInt64(-2), BigInt::fromInt64(63))->toString() == u"-9223372036854775808");
  CHECK((*BigInt::fromString(u"9007199254740993"))->toDouble() == 9007199254740992.0);
  CHECK((*BigInt::fromString(u"9007199254740995"))->toDouble() == 9007199254740996.0);
  CHECK(BigInt::fromDouble(1e21)->toString() == u"1000000000000000000000");
  CHECK(!BigInt::fromString(u"1n"));
  CHECK(!BigInt::fromString(u"-0x1"));
  CHECK((*BigInt::fromString(u"  0x10 "))->toString() == u"16");
  CHECK(*BigInt::compareToNumber(BigInt::fromInt64(1), 1.5) == -1);
  CHECK(*BigInt::compareToNumber(BigInt::fromInt64(2), 1.5) == 1);

  // Property order: indices ascending, then strings, then symbols in creation order.
  Object* o = OrdinaryObjectCreate(realm->intrinsic(Intrinsic::ObjectPrototype));
  Symbol* sym = make<Symbol>(Value(atom(u"s")));
  (void)CreateDataProperty(o, PropertyKey(u"b"), Value::number(1));
  (void)CreateDataProperty(o, PropertyKey(sym), Value::number(2));
  (void)CreateDataProperty(o, PropertyKey(u"10"), Value::number(3));
  (void)CreateDataProperty(o, PropertyKey(u"a"), Value::number(4));
  (void)CreateDataProperty(o, PropertyKey(u"2"), Value::number(5));
  (void)CreateDataProperty(o, PropertyKey(u"4294967295"), Value::number(6));
  auto keys = o->OwnPropertyKeys().release();
  CHECK(keys.size() == 6);
  CHECK(keys[0] == PropertyKey::index(2));
  CHECK(keys[1] == PropertyKey::index(10));
  CHECK(keys[2] == PropertyKey(u"b"));
  CHECK(keys[3] == PropertyKey(u"a"));
  CHECK(keys[4] == PropertyKey(u"4294967295"));
  CHECK(keys[4].isString());
  CHECK(keys[5] == PropertyKey(sym));
  (void)o->Delete(PropertyKey(u"b"));
  (void)CreateDataProperty(o, PropertyKey(u"b"), Value::number(1));
  keys = o->OwnPropertyKeys().release();
  CHECK(keys[2] == PropertyKey(u"a"));
  CHECK(keys[4] == PropertyKey(u"b"));
  for (int i = 0; i < 40; i++) (void)CreateDataProperty(o, PropertyKey(newStringFromUtf8("k" + std::to_string(i))), Value::number(i));
  CHECK(Get(o, PropertyKey(u"k33")).release().asNumber() == 33);
  CHECK(Get(o, PropertyKey(u"a")).release().asNumber() == 4);

  // Arrays
  Object* arr = ArrayCreate(0).release();
  (void)CreateDataProperty(arr, PropertyKey::index(5), Value::number(1));
  CHECK(LengthOfArrayLike(arr).release() == 6);
  (void)Set(arr, PropertyKey(u"length"), Value::number(2), true);
  CHECK(LengthOfArrayLike(arr).release() == 2);
  CHECK(!HasOwnProperty(arr, PropertyKey::index(5)).release());
  (void)CreateDataProperty(arr, PropertyKey::index(0), Value::number(1));
  (void)DefinePropertyOrThrow(arr, PropertyKey::index(1), PropertyDescriptor::data(Value::number(2), Attr::Writable));
  (void)CreateDataProperty(arr, PropertyKey::index(3), Value::number(3));
  auto r = arr->Set(PropertyKey(u"length"), Value::number(0), arr);
  CHECK(r.isNormal() && !r.value());
  CHECK(LengthOfArrayLike(arr).release() == 2);
  auto bad = arr->Set(PropertyKey(u"length"), Value::number(1.5), arr);
  CHECK(bad.isThrow());
  Object* big = ArrayCreate(0).release();
  (void)CreateDataProperty(big, PropertyKey::index(4294967294u), Value::number(1));
  CHECK(LengthOfArrayLike(big).release() == 4294967295.0);
  CHECK(!CreateDataProperty(big, PropertyKey(u"4294967295"), Value::number(1)).release() == false);

  CHECK(heap().cellCount() > 0);
  if (failures) std::printf("%d failures\n", failures);
  else std::printf("all core checks passed\n");
  return failures ? 1 : 0;
}
