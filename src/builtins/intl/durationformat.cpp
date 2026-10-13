// Intl.DurationFormat (ECMA-402 13): the unit styles and displays, ToDurationRecord, and
// PartitionDurationFormatPattern over Intl.NumberFormat and Intl.ListFormat.
#include <cmath>

#include "builtins/intl/intl.h"
#include "builtins/intl/services.h"
#include "builtins/temporal/temporal.h"
#include "runtime/bigint.h"

namespace snowman::intl {

namespace {

constexpr const char16_t* kUnits[] = {u"years", u"months", u"weeks", u"days", u"hours", u"minutes", u"seconds",
                                      u"milliseconds", u"microseconds", u"nanoseconds"};
constexpr int kUnitCount = 10;

struct UnitOptions {
  std::string style, display;
};

class DurationFormatObject : public Object {
 public:
  using Object::Object;
  std::string locale, dataLocale, numberingSystem, style;
  UnitOptions units[kUnitCount];
  std::optional<int> fractionalDigits;
};

constexpr const char* kRelevantKeys[] = {"nu"};
std::vector<std::string> durationKeyValues(const std::string& locale, std::string_view key) {
  if (key != "nu") return {};
  std::vector<std::string> out{data::defaultNumberingSystem(locale)};
  for (const std::string& nu : data::numberingSystems())
    if (nu != out[0]) out.push_back(nu);
  return out;
}
const ServiceData kDurationFormatData{data::locales, durationKeyValues};

// 13.5.6 GetDurationUnitOptions ( unit, options, baseStyle, stylesList, digitalBase, prevStyle, twoDigitHours )
Result<UnitOptions> GetDurationUnitOptions(int unit, Object* options, const std::string& baseStyle, const std::string& prevStyle,
                                           bool twoDigitHours) {
  std::u16string_view name = kUnits[unit];
  bool timeUnit = unit >= 4 && unit <= 6;  // hours, minutes, seconds
  std::optional<std::string> style;
  if (unit < 4) style = TRY(GetStringOption(options, name, {"long", "short", "narrow"}));
  else if (unit < 7) style = TRY(GetStringOption(options, name, {"long", "short", "narrow", "numeric", "2-digit"}));
  else style = TRY(GetStringOption(options, name, {"long", "short", "narrow", "numeric"}));
  std::string displayDefault = "always";
  if (!style) {
    if (baseStyle == "digital") {
      if (!timeUnit) displayDefault = "auto";
      style = unit < 4 ? "short" : "numeric";
    } else if (prevStyle == "fractional" || prevStyle == "numeric" || prevStyle == "2-digit") {
      if (unit != 5 && unit != 6) displayDefault = "auto";
      style = "numeric";
    } else {
      displayDefault = "auto";
      style = baseStyle;
    }
  }
  if (*style == "numeric" && unit >= 7) {
    style = "fractional";
    displayDefault = "auto";
  }
  std::u16string displayField(name);
  displayField += u"Display";
  std::string display = *TRY(GetStringOption(options, displayField, {"auto", "always"}, displayDefault));
  // ValidateDurationUnitStyle ( unit, style, display, prevStyle )
  if (display == "always" && *style == "fractional") return ThrowRangeError("Intl.DurationFormat: a fractional unit cannot always display");
  if (prevStyle == "fractional" && *style != "fractional") return ThrowRangeError("Intl.DurationFormat: unit after a fractional one must be fractional");
  if ((prevStyle == "numeric" || prevStyle == "2-digit") && *style != "fractional" && *style != "numeric" && *style != "2-digit")
    return ThrowRangeError("Intl.DurationFormat: unit after a numeric one must be numeric");
  if (unit == 4 && twoDigitHours) style = "2-digit";
  if ((unit == 5 || unit == 6) && (prevStyle == "numeric" || prevStyle == "2-digit")) style = "2-digit";
  return UnitOptions{*style, display};
}

// 13.1.1 Intl.DurationFormat ( [ locales [ , options ] ] )
Result<Value> DurationFormat_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Intl.DurationFormat requires 'new'");
  DurationFormatObject* df = TRY(OrdinaryCreateFromConstructor<DurationFormatObject>(call.newTarget, Intrinsic::IntlDurationFormatPrototype));
  std::vector<std::string> requested = TRY(CanonicalizeLocaleList(call.args[0]));
  Object* options = TRY(GetOptionsObject(call.args[1]));
  LocaleOptions opt;
  opt.localeMatcher = TRY(GetLocaleMatcher(options));
  std::optional<std::string> numberingSystem = TRY(GetStringOption(options, u"numberingSystem", {}));
  if (numberingSystem && !isUnicodeLocaleType(*numberingSystem))
    return ThrowRangeError("Intl.DurationFormat: invalid numberingSystem " + *numberingSystem);
  if (numberingSystem) opt.keys.emplace_back("nu", LocaleOption::string(*numberingSystem));
  ResolvedLocale r = ResolveLocale(kDurationFormatData, requested, opt, kRelevantKeys);
  df->locale = r.locale;
  df->dataLocale = r.dataLocale;
  df->numberingSystem = r.get("nu");
  df->style = *TRY(GetStringOption(options, u"style", {"long", "short", "narrow", "digital"}, "short"));
  std::string prevStyle;
  for (int u = 0; u < kUnitCount; u++) {
    df->units[u] = TRY(GetDurationUnitOptions(u, options, df->style, prevStyle, false));
    if (u >= 4 && u <= 8) prevStyle = df->units[u].style;
  }
  std::optional<double> fd = TRY(GetNumberOption(options, u"fractionalDigits", 0, 9, std::nullopt));
  if (fd) df->fractionalDigits = int(*fd);
  return Value(df);
}

Result<Value> DurationFormat_supportedLocalesOf(NativeCall& call) { return supportedLocalesOf(kDurationFormatData, call); }

// ToIntegerIfIntegral ( argument )
Result<double> toIntegerIfIntegral(Value v) {
  double d = TRY(ToNumber(v));
  if (!std::isfinite(d) || std::floor(d) != d) return ThrowRangeError("Intl.DurationFormat: duration fields must be integers");
  return d == 0 ? 0.0 : d;
}

// 13.5.3 ToDurationRecord ( input ): the fields in kUnits order. A Temporal.Duration gives its own fields and a
// String is parsed as an ISO 8601 duration (ToTemporalDuration).
Result<std::array<double, kUnitCount>> ToDurationRecord(Value input) {
  temporal::DurationObject* duration = temporal::asTemporal<temporal::DurationObject>(input);
  if (!duration && input.isString()) duration = TRY(temporal::ToTemporalDuration(input));
  if (duration) {
    const temporal::DurationFields& f = duration->fields;
    return std::array<double, kUnitCount>{f.years, f.months, f.weeks, f.days, f.hours, f.minutes, f.seconds,
                                          f.milliseconds, f.microseconds, f.nanoseconds};
  }
  if (!input.isObject()) {
    if (input.isString()) return ThrowRangeError("Intl.DurationFormat: a duration string is not supported");
    return ThrowTypeError("Intl.DurationFormat: duration must be an object");
  }
  Object* o = input.asObject();
  std::array<double, kUnitCount> r{};
  // Read in alphabetical order: days hours microseconds milliseconds minutes months nanoseconds seconds weeks years.
  static const int order[] = {3, 4, 8, 7, 5, 1, 9, 6, 2, 0};
  bool any = false;
  for (int u : order) {
    Value v = TRY(Get(o, PropertyKey(kUnits[u])));
    if (v.isUndefined()) continue;
    any = true;
    r[size_t(u)] = TRY(toIntegerIfIntegral(v));
  }
  if (!any) return ThrowTypeError("Intl.DurationFormat: duration has no fields");
  // IsValidDuration
  int sign = 0;
  for (double v : r) {
    int s = (v > 0) - (v < 0);
    if (s && sign && s != sign) return ThrowRangeError("Intl.DurationFormat: mixed-sign duration");
    if (s) sign = s;
  }
  if (std::fabs(r[0]) >= 4294967296.0 || std::fabs(r[1]) >= 4294967296.0 || std::fabs(r[2]) >= 4294967296.0)
    return ThrowRangeError("Intl.DurationFormat: duration out of range");
  // The normalized seconds |days·86400 + hours·3600 + ... + ns·1e-9| must be < 2^53, computed exactly in
  // nanoseconds (a field above 1e25 is out of range on its own).
  static const long long nsPerUnit[] = {86400000000000LL, 3600000000000LL, 60000000000LL, 1000000000LL, 1000000LL, 1000LL, 1LL};
  __int128 total = 0;
  for (int u = 3; u < kUnitCount; u++) {
    if (std::fabs(r[size_t(u)]) > 1e25) return ThrowRangeError("Intl.DurationFormat: duration out of range");
    total += (__int128)r[size_t(u)] * nsPerUnit[u - 3];
  }
  if (total < 0) total = -total;
  if (total >= (__int128)9007199254740992LL * 1000000000LL) return ThrowRangeError("Intl.DurationFormat: duration out of range");
  return r;
}

// The exact decimal of seconds + ms/1e3 + us/1e6 + ns/1e9 (from `unit` on), for the numeric fractional display.
Decimal fractionalValue(const std::array<double, kUnitCount>& d, int unit) {
  // Sum in nanoseconds-of-unit using BigInt-free decimal arithmetic on doubles that are exact integers.
  // The exact value of an integral double (not its shortest decimal form).
  auto dec = [](double v) {
    Decimal x = Decimal::fromBigInt(BigInt::fromDouble(v));
    x.negative = std::signbit(v);
    return x;
  };
  Decimal total = dec(d[size_t(unit)]);
  int exponent = 0;
  for (int u = unit + 1; u <= 9; u++) {
    exponent -= 3;
    Decimal part = dec(d[size_t(u)]).scaled(exponent);
    // total += part (same sign or zero: valid durations do not mix signs).
    if (part.isZero()) continue;
    if (total.isZero()) {
      bool negative = total.negative || part.negative;
      total = part;
      total.negative = negative;
      continue;
    }
    // Align exponents and add digit strings.
    int64_t e = std::min(total.exponent, part.exponent);
    std::string a = total.digits + std::string(size_t(total.exponent - e), '0');
    std::string b = part.digits + std::string(size_t(part.exponent - e), '0');
    if (a.size() < b.size()) std::swap(a, b);
    b = std::string(a.size() - b.size(), '0') + b;
    std::string sum(a.size() + 1, '0');
    int carry = 0;
    for (size_t i = a.size(); i-- > 0;) {
      int v = (a[i] - '0') + (b[i] - '0') + carry;
      sum[i + 1] = char('0' + v % 10);
      carry = v / 10;
    }
    sum[0] = char('0' + carry);
    bool negative = total.negative;
    total = Decimal::fromIntegerString(sum, negative);
    total.exponent += e;
    total.normalize();
  }
  return total;
}

struct DurationPart {
  const char16_t* type;
  std::u16string value;
  const char16_t* unit;  // singular, nullptr for list literals
};

// 13.5.7 PartitionDurationFormatPattern ( durationFormat, duration )
Result<std::vector<DurationPart>> partitionDuration(const DurationFormatObject& df, const std::array<double, kUnitCount>& duration) {
  static const char16_t* const singular[] = {u"year", u"month", u"week", u"day", u"hour", u"minute", u"second",
                                             u"millisecond", u"microsecond", u"nanosecond"};
  std::vector<std::vector<DurationPart>> result;
  bool needSeparator = false, displayNegativeSign = true;
  bool negativeDuration = false;
  for (double v : duration) negativeDuration |= v < 0;
  for (int u = 0; u < kUnitCount; u++) {
    Decimal value = Decimal::fromDouble(duration[size_t(u)]);
    std::string style = df.units[u].style;
    const std::string& display = df.units[u].display;
    Object* nfOpts = OrdinaryObjectCreate(nullptr);
    bool done = false;
    if (u >= 6 && u <= 8 && u + 1 < kUnitCount && df.units[u + 1].style == "fractional") {
      value = fractionalValue(duration, u);
      int fd = df.fractionalDigits.value_or(-1);
      TRY(CreateDataPropertyOrThrow(nfOpts, PropertyKey(u"maximumFractionDigits"), num(fd >= 0 ? fd : 9)));
      TRY(CreateDataPropertyOrThrow(nfOpts, PropertyKey(u"minimumFractionDigits"), num(fd >= 0 ? fd : 0)));
      TRY(CreateDataPropertyOrThrow(nfOpts, PropertyKey(u"roundingMode"), str(u"trunc")));
      done = true;
    }
    bool displayRequired = false;
    if (u == 5 && needSeparator) {
      displayRequired = df.units[6].display == "always" || duration[6] != 0 || duration[7] != 0 || duration[8] != 0 || duration[9] != 0;
    }
    if (!value.isZero() || display != "auto" || displayRequired) {
      if (displayNegativeSign) {
        displayNegativeSign = false;
        if (value.isZero() && negativeDuration) value.negative = true;
      } else {
        TRY(CreateDataPropertyOrThrow(nfOpts, PropertyKey(u"signDisplay"), str(u"never")));
      }
      TRY(CreateDataPropertyOrThrow(nfOpts, PropertyKey(u"numberingSystem"), jsString(df.numberingSystem)));
      if (style == "2-digit") TRY(CreateDataPropertyOrThrow(nfOpts, PropertyKey(u"minimumIntegerDigits"), num(2)));
      bool numeric = style == "numeric" || style == "2-digit" || style == "fractional";
      if (!numeric) {
        TRY(CreateDataPropertyOrThrow(nfOpts, PropertyKey(u"style"), str(u"unit")));
        TRY(CreateDataPropertyOrThrow(nfOpts, PropertyKey(u"unit"), str(singular[u])));
        TRY(CreateDataPropertyOrThrow(nfOpts, PropertyKey(u"unitDisplay"), jsString(style)));
      } else {
        TRY(CreateDataPropertyOrThrow(nfOpts, PropertyKey(u"useGrouping"), Value::boolean(false)));
      }
      NumberFormatObject* nf = TRY(createNumberFormat(jsString(df.locale), Value(nfOpts)));
      std::vector<DurationPart> list;
      if (needSeparator) {
        result.back().push_back({u"literal", u":", nullptr});
      }
      std::vector<DurationPart>& target = needSeparator ? result.back() : list;
      for (NumberPart& p : PartitionNumberPattern(*nf, value)) target.push_back({p.type, std::move(p.value), singular[u]});
      if (!needSeparator) {
        if (style == "2-digit" || style == "numeric") needSeparator = true;
        result.push_back(std::move(list));
      }
    }
    if (done) break;
  }
  ListFormatObject lf(nullptr);
  lf.locale = df.locale;
  lf.type = "unit";
  lf.style = df.style == "digital" ? "short" : df.style;
  std::vector<std::u16string> strings;
  for (const auto& parts : result) {
    std::u16string s;
    for (const DurationPart& p : parts) s += p.value;
    strings.push_back(std::move(s));
  }
  std::vector<DurationPart> flattened;
  size_t next = 0;
  for (ListPart& p : CreatePartsFromList(lf, strings)) {
    if (p.element) {
      for (DurationPart& part : result[next++]) flattened.push_back(std::move(part));
    } else {
      flattened.push_back({u"literal", std::move(p.value), nullptr});
    }
  }
  return flattened;
}

Result<Value> DurationFormatPrototype_format(NativeCall& call) {
  DurationFormatObject* df = TRY(thisIntlObject<DurationFormatObject>(call.thisValue, "Intl.DurationFormat.prototype.format"));
  std::array<double, kUnitCount> record = TRY(ToDurationRecord(call.args[0]));
  std::vector<DurationPart> parts = TRY(partitionDuration(*df, record));
  std::u16string out;
  for (const DurationPart& p : parts) out += p.value;
  return Value(newString(std::move(out)));
}

Result<Value> DurationFormatPrototype_formatToParts(NativeCall& call) {
  DurationFormatObject* df = TRY(thisIntlObject<DurationFormatObject>(call.thisValue, "Intl.DurationFormat.prototype.formatToParts"));
  std::array<double, kUnitCount> record = TRY(ToDurationRecord(call.args[0]));
  std::vector<DurationPart> parts = TRY(partitionDuration(*df, record));
  ValueVector values;
  for (const DurationPart& p : parts) {
    Object* o = builtins::newPlainObject();
    addProperty(o, u"type", str(p.type));
    addProperty(o, u"value", Value(newString(p.value)));
    if (p.unit) addProperty(o, u"unit", str(p.unit));
    values.push_back(Value(o));
  }
  return Value(CreateArrayFromList(values));
}

Result<Value> DurationFormatPrototype_resolvedOptions(NativeCall& call) {
  DurationFormatObject* df = TRY(thisIntlObject<DurationFormatObject>(call.thisValue, "Intl.DurationFormat.prototype.resolvedOptions"));
  Object* o = builtins::newPlainObject();
  addProperty(o, u"locale", jsString(df->locale));
  addProperty(o, u"numberingSystem", jsString(df->numberingSystem));
  addProperty(o, u"style", jsString(df->style));
  for (int u = 0; u < kUnitCount; u++) {
    const std::string& style = df->units[u].style;
    addProperty(o, kUnits[u], jsString(style == "fractional" ? "numeric" : style));
    std::u16string display(kUnits[u]);
    display += u"Display";
    addProperty(o, display, jsString(df->units[u].display));
  }
  if (df->fractionalDigits) addProperty(o, u"fractionalDigits", num(*df->fractionalDigits));
  return Value(o);
}

}  // namespace

void installDurationFormat(Realm& realm, Object* intl) {
  Object* proto = nullptr;
  BuiltinFunction* ctor = defineIntlConstructor(realm, intl, Intrinsic::IntlDurationFormat, DurationFormat_construct, 0,
                                                u"DurationFormat", Intrinsic::IntlDurationFormatPrototype, &proto);
  defineFunction(realm, ctor, PropertyKey(u"supportedLocalesOf"), 1, DurationFormat_supportedLocalesOf);
  defineFunction(realm, proto, PropertyKey(u"format"), 1, DurationFormatPrototype_format);
  defineFunction(realm, proto, PropertyKey(u"formatToParts"), 1, DurationFormatPrototype_formatToParts);
  defineFunction(realm, proto, PropertyKey(u"resolvedOptions"), 0, DurationFormatPrototype_resolvedOptions);
}

}  // namespace snowman::intl
