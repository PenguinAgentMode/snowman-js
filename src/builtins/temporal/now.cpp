// Temporal.Now (proposal-temporal §2): the current instant, date and time in the system or a given time zone.
#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

using builtins::str;

namespace {

// SystemDateTime
Result<ISODateTime> SystemDateTime(Value temporalTimeZoneLike) {
  TimeZone timeZone = temporalTimeZoneLike.isUndefined() ? SystemTimeZoneIdentifier()
                                                         : TRY(ToTemporalTimeZoneIdentifier(temporalTimeZoneLike));
  return GetISODateTimeFor(timeZone, SystemUTCEpochNanoseconds());
}

Result<Value> Now_timeZoneId(NativeCall&) { return Value(newString(TimeZoneIdentifier(SystemTimeZoneIdentifier()))); }

Result<Value> Now_instant(NativeCall&) { return Value(TRY(CreateTemporalInstant(SystemUTCEpochNanoseconds()))); }

Result<Value> Now_plainDateTimeISO(NativeCall& call) {
  ISODateTime isoDateTime = TRY(SystemDateTime(call.args[0]));
  return Value(TRY(CreateTemporalDateTime(isoDateTime, IsoCalendar())));
}

Result<Value> Now_zonedDateTimeISO(NativeCall& call) {
  TimeZone timeZone = call.args[0].isUndefined() ? SystemTimeZoneIdentifier() : TRY(ToTemporalTimeZoneIdentifier(call.args[0]));
  return Value(TRY(CreateTemporalZonedDateTime(SystemUTCEpochNanoseconds(), timeZone, IsoCalendar())));
}

Result<Value> Now_plainDateISO(NativeCall& call) {
  ISODateTime isoDateTime = TRY(SystemDateTime(call.args[0]));
  return Value(TRY(CreateTemporalDate(isoDateTime.isoDate, IsoCalendar())));
}

Result<Value> Now_plainTimeISO(NativeCall& call) {
  ISODateTime isoDateTime = TRY(SystemDateTime(call.args[0]));
  return Value(TRY(CreateTemporalTime(isoDateTime.time)));
}

void installNow(Realm& realm) {
  Object* now = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  realm.setIntrinsic(Intrinsic::TemporalNow, now);
  defineValue(TemporalNamespace(realm), PropertyKey(u"Now"), Value(now));
  defineFunction(realm, now, PropertyKey(u"timeZoneId"), 0, Now_timeZoneId);
  defineFunction(realm, now, PropertyKey(u"instant"), 0, Now_instant);
  defineFunction(realm, now, PropertyKey(u"plainDateTimeISO"), 0, Now_plainDateTimeISO);
  defineFunction(realm, now, PropertyKey(u"zonedDateTimeISO"), 0, Now_zonedDateTimeISO);
  defineFunction(realm, now, PropertyKey(u"plainDateISO"), 0, Now_plainDateISO);
  defineFunction(realm, now, PropertyKey(u"plainTimeISO"), 0, Now_plainTimeISO);
  defineValue(now, PropertyKey(builtins::symbols().toStringTag), str(u"Temporal.Now"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(TemporalNow, kOrderDefault, installNow);

// Weak fallbacks until ws/temporal's PlainTime and PlainDateTime land (their strong definitions replace these).
__attribute__((weak)) Result<PlainTimeObject*> CreateTemporalTime(const TimeRecord& time, Object* newTarget) {
  Object* prototype = newTarget ? TRY(GetPrototypeFromConstructor(newTarget, Intrinsic::TemporalPlainTimePrototype))
                                : builtins::intrinsic(Intrinsic::TemporalPlainTimePrototype);
  if (!prototype) prototype = builtins::intrinsic(Intrinsic::ObjectPrototype);
  return make<PlainTimeObject>(prototype, time);
}
__attribute__((weak)) Result<PlainDateTimeObject*> CreateTemporalDateTime(const ISODateTime& isoDateTime,
                                                                          const CalendarId& calendar, Object* newTarget) {
  Object* prototype = newTarget ? TRY(GetPrototypeFromConstructor(newTarget, Intrinsic::TemporalPlainDateTimePrototype))
                                : builtins::intrinsic(Intrinsic::TemporalPlainDateTimePrototype);
  if (!prototype) prototype = builtins::intrinsic(Intrinsic::ObjectPrototype);
  return make<PlainDateTimeObject>(prototype, isoDateTime, calendar);
}

}  // namespace snowman::temporal
