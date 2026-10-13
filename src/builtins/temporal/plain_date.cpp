// Temporal.PlainDate (proposal-temporal §3).
#include <cmath>

#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

Result<PlainDateObject*> CreateTemporalDate(const ISODate& isoDate, const CalendarId& calendar, Object* newTarget) {
  if (!ISODateWithinLimits(isoDate)) return ThrowRangeError("date is outside the supported range");
  if (!newTarget) newTarget = builtins::intrinsic(Intrinsic::TemporalPlainDate);
  if (!newTarget) return make<PlainDateObject>(builtins::intrinsic(Intrinsic::ObjectPrototype), isoDate, calendar);
  return OrdinaryCreateFromConstructor<PlainDateObject>(newTarget, Intrinsic::TemporalPlainDatePrototype, isoDate, calendar);
}

}  // namespace snowman::temporal
