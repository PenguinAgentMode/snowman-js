// Temporal.Duration (proposal-temporal §7) and the duration abstract operations shared by every Temporal
// type: internal duration records, balancing, RoundRelativeDuration and its nudges, relativeTo.
#include <cmath>

#include "builtins/temporal/temporal.h"

namespace snowman::temporal {

using builtins::num;

// ---------------------------------------------------------------------------------------------------------
// Validity, sign, largest unit

bool IsValidDuration(const DurationFields& d) {
  const double values[] = {d.years, d.months, d.weeks, d.days, d.hours, d.minutes, d.seconds, d.milliseconds,
                           d.microseconds, d.nanoseconds};
  int sign = 0;
  for (double v : values) {
    if (!std::isfinite(v)) return false;
    if (v < 0) {
      if (sign > 0) return false;
      sign = -1;
    } else if (v > 0) {
      if (sign < 0) return false;
      sign = 1;
    }
  }
  constexpr double two32 = 4294967296.0, two53 = 9007199254740992.0;
  if (std::fabs(d.years) >= two32 || std::fabs(d.months) >= two32 || std::fabs(d.weeks) >= two32) return false;
  // Each time component alone must stay below 2^53 seconds (they all have the same sign), which also keeps the
  // exact sum below within 128 bits.
  if (std::fabs(d.days) > two53 / 86400 || std::fabs(d.hours) > two53 / 3600 || std::fabs(d.minutes) > two53 / 60 ||
      std::fabs(d.seconds) > two53 || std::fabs(d.milliseconds) > two53 * 1e3 || std::fabs(d.microseconds) > two53 * 1e6 ||
      std::fabs(d.nanoseconds) > two53 * 1e9)
    return false;
  Int128 ns = TimeDurationFromComponents(d.hours, d.minutes, d.seconds, d.milliseconds, d.microseconds, d.nanoseconds) +
              DoubleToInt128(d.days) * nsPerDay;
  Int128 limit = Int128(9007199254740992) * nsPerSecond;
  return (ns < 0 ? -ns : ns) < limit;
}

int DurationSign(const DurationFields& d) {
  const double values[] = {d.years, d.months, d.weeks, d.days, d.hours, d.minutes, d.seconds, d.milliseconds,
                           d.microseconds, d.nanoseconds};
  for (double v : values) {
    if (v < 0) return -1;
    if (v > 0) return 1;
  }
  return 0;
}

int DateDurationSign(const DateDuration& d) {
  const int64_t values[] = {d.years, d.months, d.weeks, d.days};
  for (int64_t v : values) {
    if (v < 0) return -1;
    if (v > 0) return 1;
  }
  return 0;
}

int InternalDurationSign(const InternalDuration& d) {
  int dateSign = DateDurationSign(d.date);
  if (dateSign != 0) return dateSign;
  return TimeDurationSign(d.time);
}

Unit DefaultTemporalLargestUnit(const DurationFields& d) {
  if (d.years != 0) return Unit::Year;
  if (d.months != 0) return Unit::Month;
  if (d.weeks != 0) return Unit::Week;
  if (d.days != 0) return Unit::Day;
  if (d.hours != 0) return Unit::Hour;
  if (d.minutes != 0) return Unit::Minute;
  if (d.seconds != 0) return Unit::Second;
  if (d.milliseconds != 0) return Unit::Millisecond;
  if (d.microseconds != 0) return Unit::Microsecond;
  return Unit::Nanosecond;
}

// ---------------------------------------------------------------------------------------------------------
// Creation and conversion

Result<DurationObject*> CreateTemporalDuration(const DurationFields& fields, Object* newTarget) {
  if (!IsValidDuration(fields)) return ThrowRangeError("duration is out of range or has mixed signs");
  if (!newTarget) newTarget = builtins::intrinsic(Intrinsic::TemporalDuration);
  DurationFields f = fields;
  double* all[] = {&f.years, &f.months, &f.weeks, &f.days, &f.hours, &f.minutes, &f.seconds, &f.milliseconds,
                   &f.microseconds, &f.nanoseconds};
  for (double* v : all) *v = *v + 0.0;
  return OrdinaryCreateFromConstructor<DurationObject>(newTarget, Intrinsic::TemporalDurationPrototype, f);
}

DurationObject* CreateNegatedTemporalDuration(DurationObject* duration) {
  const DurationFields& d = duration->fields;
  DurationFields n{-d.years, -d.months, -d.weeks, -d.days, -d.hours, -d.minutes, -d.seconds, -d.milliseconds,
                   -d.microseconds, -d.nanoseconds};
  return CreateTemporalDuration(n).release();
}

namespace {

// Partial Duration Record: absent fields are nullopt.
struct PartialDuration {
  std::optional<double> years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds;
};

// ToTemporalPartialDurationRecord: properties are read in alphabetical order.
Result<PartialDuration> ToTemporalPartialDurationRecord(Value temporalDurationLike) {
  if (!temporalDurationLike.isObject()) return ThrowTypeError("duration-like must be an object");
  Object* o = temporalDurationLike.asObject();
  PartialDuration r;
  struct Row {
    const char16_t* name;
    std::optional<double> PartialDuration::* field;
  };
  static const Row rows[] = {
      {u"days", &PartialDuration::days},
      {u"hours", &PartialDuration::hours},
      {u"microseconds", &PartialDuration::microseconds},
      {u"milliseconds", &PartialDuration::milliseconds},
      {u"minutes", &PartialDuration::minutes},
      {u"months", &PartialDuration::months},
      {u"nanoseconds", &PartialDuration::nanoseconds},
      {u"seconds", &PartialDuration::seconds},
      {u"weeks", &PartialDuration::weeks},
      {u"years", &PartialDuration::years},
  };
  bool any = false;
  for (const Row& row : rows) {
    Value v = TRY(Get(o, PropertyKey(row.name)));
    if (!v.isUndefined()) {
      r.*row.field = TRY(ToIntegerIfIntegral(v));
      any = true;
    }
  }
  if (!any) return ThrowTypeError("duration-like object must have at least one duration property");
  return r;
}

}  // namespace

Result<DurationObject*> ToTemporalDuration(Value item) {
  if (DurationObject* d = asTemporal<DurationObject>(item)) return CreateTemporalDuration(d->fields);
  if (!item.isObject()) {
    if (!item.isString()) return ThrowTypeError("duration must be a string or an object");
    DurationFields fields = TRY(ParseTemporalDurationString(item.asString()->view()));
    return CreateTemporalDuration(fields);
  }
  PartialDuration partial = TRY(ToTemporalPartialDurationRecord(item));
  DurationFields result;
  result.years = partial.years.value_or(0);
  result.months = partial.months.value_or(0);
  result.weeks = partial.weeks.value_or(0);
  result.days = partial.days.value_or(0);
  result.hours = partial.hours.value_or(0);
  result.minutes = partial.minutes.value_or(0);
  result.seconds = partial.seconds.value_or(0);
  result.milliseconds = partial.milliseconds.value_or(0);
  result.microseconds = partial.microseconds.value_or(0);
  result.nanoseconds = partial.nanoseconds.value_or(0);
  return CreateTemporalDuration(result);
}

Result<DateDuration> CreateDateDurationRecord(int64_t years, int64_t months, int64_t weeks, int64_t days) {
  DurationFields f;
  f.years = double(years);
  f.months = double(months);
  f.weeks = double(weeks);
  f.days = double(days);
  if (!IsValidDuration(f)) return ThrowRangeError("date duration is out of range");
  return DateDuration{years, months, weeks, days};
}

Result<DateDuration> AdjustDateDurationRecord(const DateDuration& d, int64_t days, std::optional<int64_t> weeks,
                                              std::optional<int64_t> months) {
  return CreateDateDurationRecord(d.years, months.value_or(d.months), weeks.value_or(d.weeks), days);
}

InternalDuration CombineDateAndTimeDuration(const DateDuration& date, TimeDuration time) { return InternalDuration{date, time}; }

InternalDuration ToInternalDurationRecord(const DurationFields& d) {
  DateDuration date{int64_t(d.years), int64_t(d.months), int64_t(d.weeks), int64_t(d.days)};
  TimeDuration time = TimeDurationFromComponents(d.hours, d.minutes, d.seconds, d.milliseconds, d.microseconds, d.nanoseconds);
  return InternalDuration{date, time};
}

InternalDuration ToInternalDurationRecordWith24HourDays(const DurationFields& d) {
  TimeDuration time = TimeDurationFromComponents(d.hours, d.minutes, d.seconds, d.milliseconds, d.microseconds, d.nanoseconds);
  time += DoubleToInt128(d.days) * nsPerDay;
  DateDuration date{int64_t(d.years), int64_t(d.months), int64_t(d.weeks), 0};
  return InternalDuration{date, time};
}

DateDuration ToDateDurationRecordWithoutTime(const DurationFields& d) {
  InternalDuration internal = ToInternalDurationRecordWith24HourDays(d);
  int64_t days = int64_t(internal.time / nsPerDay);
  return DateDuration{internal.date.years, internal.date.months, internal.date.weeks, days};
}

Result<DurationFields> TemporalDurationFieldsFromInternal(const InternalDuration& internalDuration, Unit largestUnit) {
  Int128 days = 0, hours = 0, minutes = 0, seconds = 0, milliseconds = 0, microseconds = 0;
  int sign = TimeDurationSign(internalDuration.time);
  Int128 nanoseconds = internalDuration.time < 0 ? -internalDuration.time : internalDuration.time;
  auto split = [](Int128& small, Int128& big, int64_t factor) {
    big = small / factor;
    small = small % factor;
  };
  if (IsDateUnit(largestUnit)) {
    split(nanoseconds, microseconds, 1000);
    split(microseconds, milliseconds, 1000);
    split(milliseconds, seconds, 1000);
    split(seconds, minutes, 60);
    split(minutes, hours, 60);
    split(hours, days, 24);
  } else if (largestUnit == Unit::Hour) {
    split(nanoseconds, microseconds, 1000);
    split(microseconds, milliseconds, 1000);
    split(milliseconds, seconds, 1000);
    split(seconds, minutes, 60);
    split(minutes, hours, 60);
  } else if (largestUnit == Unit::Minute) {
    split(nanoseconds, microseconds, 1000);
    split(microseconds, milliseconds, 1000);
    split(milliseconds, seconds, 1000);
    split(seconds, minutes, 60);
  } else if (largestUnit == Unit::Second) {
    split(nanoseconds, microseconds, 1000);
    split(microseconds, milliseconds, 1000);
    split(milliseconds, seconds, 1000);
  } else if (largestUnit == Unit::Millisecond) {
    split(nanoseconds, microseconds, 1000);
    split(microseconds, milliseconds, 1000);
  } else if (largestUnit == Unit::Microsecond) {
    split(nanoseconds, microseconds, 1000);
  }
  const DateDuration& date = internalDuration.date;
  DurationFields f;
  f.years = double(date.years);
  f.months = double(date.months);
  f.weeks = double(date.weeks);
  f.days = Int128ToDouble(Int128(date.days) + days * sign);
  f.hours = Int128ToDouble(hours * sign);
  f.minutes = Int128ToDouble(minutes * sign);
  f.seconds = Int128ToDouble(seconds * sign);
  f.milliseconds = Int128ToDouble(milliseconds * sign);
  f.microseconds = Int128ToDouble(microseconds * sign);
  f.nanoseconds = Int128ToDouble(nanoseconds * sign);
  if (!IsValidDuration(f)) return ThrowRangeError("duration is out of range");
  return f;
}

Result<DurationObject*> TemporalDurationFromInternal(const InternalDuration& d, Unit largestUnit, Object* newTarget) {
  DurationFields f = TRY(TemporalDurationFieldsFromInternal(d, largestUnit));
  return CreateTemporalDuration(f, newTarget);
}

// ---------------------------------------------------------------------------------------------------------
// Differences and relative rounding

InternalDuration DifferenceISODateTime(const ISODateTime& isoDateTime1, const ISODateTime& isoDateTime2,
                                       const CalendarId& calendar, Unit largestUnit) {
  TimeDuration timeDuration = DifferenceTime(isoDateTime1.time, isoDateTime2.time);
  int timeSign = TimeDurationSign(timeDuration);
  int dateSign = CompareISODate(isoDateTime2.isoDate, isoDateTime1.isoDate);
  ISODate adjustedDate = isoDateTime2.isoDate;
  if (timeSign == -dateSign) {
    adjustedDate = AddDaysToISODate(adjustedDate, timeSign);
    timeDuration -= Int128(timeSign) * nsPerDay;
  }
  Unit dateLargestUnit = LargerOfTwoTemporalUnits(Unit::Day, largestUnit);
  DateDuration dateDifference = CalendarDateUntil(calendar, isoDateTime1.isoDate, adjustedDate, dateLargestUnit);
  if (largestUnit != dateLargestUnit) {
    timeDuration += Int128(dateDifference.days) * nsPerDay;
    dateDifference.days = 0;
  }
  return CombineDateAndTimeDuration(dateDifference, timeDuration);
}

namespace {

// The epoch nanoseconds of a date-time in timeZone (unset: UTC wall clock).
Result<EpochNanoseconds> epochNsFor(const TimeZone* timeZone, const ISODateTime& dt) {
  if (!timeZone) return GetUTCEpochNanoseconds(dt);
  return GetEpochNanosecondsForZone(*timeZone, dt, Disambiguation::Compatible);
}

struct CalendarNudge {
  DurationNudgeResult nudgeResult;
  double total;
};

struct NudgeWindow {
  int64_t r1, r2;
  EpochNanoseconds startEpochNs, endEpochNs;
  DateDuration startDuration, endDuration;
};

// ComputeNudgeWindow(sign, duration, originEpochNs, isoDateTime, timeZone, calendar, increment, unit, additionalShift)
Result<NudgeWindow> ComputeNudgeWindow(int sign, const InternalDuration& duration, EpochNanoseconds originEpochNs,
                                       const ISODateTime& isoDateTime, const TimeZone* timeZone, const CalendarId& calendar,
                                       int64_t increment, Unit unit, bool additionalShift) {
  NudgeWindow w;
  auto bracket = [&](int64_t value) {
    int64_t truncated = value / increment * increment;
    w.r1 = additionalShift ? truncated + increment * sign : truncated;
    w.r2 = w.r1 + increment * sign;
  };
  if (unit == Unit::Year) {
    bracket(duration.date.years);
    w.startDuration = TRY(CreateDateDurationRecord(w.r1, 0, 0, 0));
    w.endDuration = TRY(CreateDateDurationRecord(w.r2, 0, 0, 0));
  } else if (unit == Unit::Month) {
    bracket(duration.date.months);
    w.startDuration = TRY(AdjustDateDurationRecord(duration.date, 0, 0, w.r1));
    w.endDuration = TRY(AdjustDateDurationRecord(duration.date, 0, 0, w.r2));
  } else if (unit == Unit::Week) {
    DateDuration yearsMonths = TRY(AdjustDateDurationRecord(duration.date, 0, 0));
    ISODate weeksStart = TRY(CalendarDateAdd(calendar, isoDateTime.isoDate, yearsMonths, Overflow::Constrain));
    ISODate weeksEnd = AddDaysToISODate(weeksStart, duration.date.days);
    DateDuration untilResult = CalendarDateUntil(calendar, weeksStart, weeksEnd, Unit::Week);
    bracket(duration.date.weeks + untilResult.weeks);
    w.startDuration = TRY(AdjustDateDurationRecord(duration.date, 0, w.r1));
    w.endDuration = TRY(AdjustDateDurationRecord(duration.date, 0, w.r2));
  } else {
    bracket(duration.date.days);
    w.startDuration = TRY(AdjustDateDurationRecord(duration.date, w.r1));
    w.endDuration = TRY(AdjustDateDurationRecord(duration.date, w.r2));
  }
  // The reference implementation starts from the origin whenever the start duration is zero.
  if (DateDurationSign(w.startDuration) == 0) {
    w.startEpochNs = originEpochNs;
  } else {
    ISODate start = TRY(CalendarDateAdd(calendar, isoDateTime.isoDate, w.startDuration, Overflow::Constrain));
    w.startEpochNs = TRY(epochNsFor(timeZone, CombineISODateAndTimeRecord(start, isoDateTime.time)));
  }
  ISODate end = TRY(CalendarDateAdd(calendar, isoDateTime.isoDate, w.endDuration, Overflow::Constrain));
  w.endEpochNs = TRY(epochNsFor(timeZone, CombineISODateAndTimeRecord(end, isoDateTime.time)));
  return w;
}

// NudgeToCalendarUnit
Result<CalendarNudge> NudgeToCalendarUnit(int sign, const InternalDuration& duration, EpochNanoseconds originEpochNs,
                                          EpochNanoseconds destEpochNs, const ISODateTime& isoDateTime,
                                          const TimeZone* timeZone, const CalendarId& calendar, int64_t increment,
                                          Unit unit, RoundingMode roundingMode) {
  bool didExpandCalendarUnit = false;
  NudgeWindow w = TRY(ComputeNudgeWindow(sign, duration, originEpochNs, isoDateTime, timeZone, calendar, increment, unit, false));
  auto inWindow = [&](const NudgeWindow& nw) {
    return sign == 1 ? (nw.startEpochNs <= destEpochNs && destEpochNs <= nw.endEpochNs)
                     : (nw.endEpochNs <= destEpochNs && destEpochNs <= nw.startEpochNs);
  };
  if (!inWindow(w)) {
    w = TRY(ComputeNudgeWindow(sign, duration, originEpochNs, isoDateTime, timeZone, calendar, increment, unit, true));
    if (!inWindow(w)) return ThrowRangeError("rounding is out of range");
    didExpandCalendarUnit = true;
  }
  int64_t r1 = w.r1;
  EpochNanoseconds startEpochNs = w.startEpochNs, endEpochNs = w.endEpochNs;
  const DateDuration& startDuration = w.startDuration;
  const DateDuration& endDuration = w.endDuration;
  if (startEpochNs == endEpochNs) return ThrowRangeError("rounding is out of range");
  // progress = (dest − start) / (end − start), exactly; total = r1 + progress × increment × sign.
  Int128 num = destEpochNs - startEpochNs, den = endEpochNs - startEpochNs;
  if (den < 0) {
    num = -num;
    den = -den;
  }
  double total = DivideToDouble(Int128(r1) * den + num * increment * sign, den);
  UnsignedRoundingMode unsignedRoundingMode = GetUnsignedRoundingMode(roundingMode, sign < 0);
  bool roundUp;
  if (num == den) {
    roundUp = true;
  } else {
    int64_t absR1 = r1 < 0 ? -r1 : r1;
    roundUp = ApplyUnsignedRoundingModeToR2(num, den, (absR1 / increment) % 2 == 0, unsignedRoundingMode);
  }
  InternalDuration resultDuration;
  EpochNanoseconds nudgedEpochNs;
  if (roundUp) {
    didExpandCalendarUnit = true;
    resultDuration = CombineDateAndTimeDuration(endDuration, 0);
    nudgedEpochNs = endEpochNs;
  } else {
    resultDuration = CombineDateAndTimeDuration(startDuration, 0);
    nudgedEpochNs = startEpochNs;
  }
  return CalendarNudge{DurationNudgeResult{resultDuration, nudgedEpochNs, didExpandCalendarUnit}, total};
}

// NudgeToZonedTime
Result<DurationNudgeResult> NudgeToZonedTime(int sign, const InternalDuration& duration, const ISODateTime& isoDateTime,
                                             const TimeZone& timeZone, const CalendarId& calendar, int64_t increment,
                                             Unit unit, RoundingMode roundingMode) {
  ISODate start = TRY(CalendarDateAdd(calendar, isoDateTime.isoDate, duration.date, Overflow::Constrain));
  ISODateTime startDateTime = CombineISODateAndTimeRecord(start, isoDateTime.time);
  ISODate endDate = AddDaysToISODate(start, sign);
  ISODateTime endDateTime = CombineISODateAndTimeRecord(endDate, isoDateTime.time);
  EpochNanoseconds startEpochNs = TRY(GetEpochNanosecondsForZone(timeZone, startDateTime, Disambiguation::Compatible));
  EpochNanoseconds endEpochNs = TRY(GetEpochNanosecondsForZone(timeZone, endDateTime, Disambiguation::Compatible));
  TimeDuration daySpan = TimeDurationFromEpochNanosecondsDifference(endEpochNs, startEpochNs);
  Int128 unitLength = UnitLengthInNanoseconds(unit);
  TimeDuration roundedTimeDuration = TRY(RoundTimeDurationToIncrement(duration.time, unitLength * increment, roundingMode));
  TimeDuration beyondDaySpan = TRY(AddTimeDuration(roundedTimeDuration, -daySpan));
  bool didRoundBeyondDay;
  int64_t dayDelta;
  EpochNanoseconds nudgedEpochNs;
  if (TimeDurationSign(beyondDaySpan) != -sign) {
    didRoundBeyondDay = true;
    dayDelta = sign;
    roundedTimeDuration = TRY(RoundTimeDurationToIncrement(beyondDaySpan, unitLength * increment, roundingMode));
    nudgedEpochNs = AddTimeDurationToEpochNanoseconds(roundedTimeDuration, endEpochNs);
  } else {
    didRoundBeyondDay = false;
    dayDelta = 0;
    nudgedEpochNs = AddTimeDurationToEpochNanoseconds(roundedTimeDuration, startEpochNs);
  }
  DateDuration dateDuration = TRY(AdjustDateDurationRecord(duration.date, duration.date.days + dayDelta));
  return DurationNudgeResult{CombineDateAndTimeDuration(dateDuration, roundedTimeDuration), nudgedEpochNs, didRoundBeyondDay};
}

// NudgeToDayOrTime
Result<DurationNudgeResult> NudgeToDayOrTime(const InternalDuration& duration, EpochNanoseconds destEpochNs, Unit largestUnit,
                                             int64_t increment, Unit smallestUnit, RoundingMode roundingMode) {
  TimeDuration timeDuration = TRY(Add24HourDaysToTimeDuration(duration.time, duration.date.days));
  Int128 unitLength = UnitLengthInNanoseconds(smallestUnit);
  TimeDuration roundedTime = TRY(RoundTimeDurationToIncrement(timeDuration, unitLength * increment, roundingMode));
  TimeDuration diffTime = TRY(AddTimeDuration(roundedTime, -timeDuration));
  Int128 wholeDays = timeDuration / nsPerDay;
  Int128 roundedWholeDays = roundedTime / nsPerDay;
  Int128 dayDelta = roundedWholeDays - wholeDays;
  int dayDeltaSign = dayDelta < 0 ? -1 : dayDelta > 0 ? 1 : 0;
  bool didExpandDays = dayDeltaSign == TimeDurationSign(timeDuration);
  EpochNanoseconds nudgedEpochNs = AddTimeDurationToEpochNanoseconds(diffTime, destEpochNs);
  int64_t days = 0;
  TimeDuration remainder = roundedTime;
  if (IsDateUnit(largestUnit)) {
    days = int64_t(roundedWholeDays);
    remainder = roundedTime - roundedWholeDays * nsPerDay;
  }
  DateDuration dateDuration = TRY(AdjustDateDurationRecord(duration.date, days));
  return DurationNudgeResult{CombineDateAndTimeDuration(dateDuration, remainder), nudgedEpochNs, didExpandDays};
}

// BubbleRelativeDuration
Result<InternalDuration> BubbleRelativeDuration(int sign, InternalDuration duration, EpochNanoseconds nudgedEpochNs,
                                                const ISODateTime& isoDateTime, const TimeZone* timeZone,
                                                const CalendarId& calendar, Unit largestUnit, Unit smallestUnit) {
  if (smallestUnit == largestUnit) return duration;
  int largestUnitIndex = int(largestUnit);
  int smallestUnitIndex = int(smallestUnit);
  int unitIndex = smallestUnitIndex - 1;
  bool done = false;
  while (unitIndex >= largestUnitIndex && !done) {
    Unit unit = Unit(unitIndex);
    if (unit != Unit::Week || largestUnit == Unit::Week) {
      DateDuration endDuration;
      if (unit == Unit::Year) {
        endDuration = TRY(CreateDateDurationRecord(duration.date.years + sign, 0, 0, 0));
      } else if (unit == Unit::Month) {
        endDuration = TRY(AdjustDateDurationRecord(duration.date, 0, 0, duration.date.months + sign));
      } else {
        endDuration = TRY(AdjustDateDurationRecord(duration.date, 0, duration.date.weeks + sign));
      }
      ISODate end = TRY(CalendarDateAdd(calendar, isoDateTime.isoDate, endDuration, Overflow::Constrain));
      EpochNanoseconds endEpochNs = TRY(epochNsFor(timeZone, CombineISODateAndTimeRecord(end, isoDateTime.time)));
      Int128 beyondEnd = nudgedEpochNs - endEpochNs;
      int beyondEndSign = beyondEnd < 0 ? -1 : beyondEnd > 0 ? 1 : 0;
      if (beyondEndSign != -sign) duration = CombineDateAndTimeDuration(endDuration, 0);
      else done = true;
    }
    unitIndex--;
  }
  return duration;
}

}  // namespace

Result<InternalDuration> RoundRelativeDuration(const InternalDuration& duration, EpochNanoseconds originEpochNs,
                                               EpochNanoseconds destEpochNs, const ISODateTime& isoDateTime,
                                               const TimeZone* timeZone, const CalendarId& calendar, Unit largestUnit,
                                               int64_t increment, Unit smallestUnit, RoundingMode roundingMode) {
  bool irregularLengthUnit = IsCalendarUnit(smallestUnit) || (timeZone && smallestUnit == Unit::Day);
  int sign = InternalDurationSign(duration) < 0 ? -1 : 1;
  DurationNudgeResult nudgeResult;
  if (irregularLengthUnit) {
    CalendarNudge record = TRY(NudgeToCalendarUnit(sign, duration, originEpochNs, destEpochNs, isoDateTime, timeZone,
                                                   calendar, increment, smallestUnit, roundingMode));
    nudgeResult = record.nudgeResult;
  } else if (timeZone) {
    nudgeResult = TRY(NudgeToZonedTime(sign, duration, isoDateTime, *timeZone, calendar, increment, smallestUnit, roundingMode));
  } else {
    nudgeResult = TRY(NudgeToDayOrTime(duration, destEpochNs, largestUnit, increment, smallestUnit, roundingMode));
  }
  InternalDuration result = nudgeResult.duration;
  if (nudgeResult.didExpandCalendarUnit && smallestUnit != Unit::Week) {
    Unit startUnit = LargerOfTwoTemporalUnits(smallestUnit, Unit::Day);
    result = TRY(BubbleRelativeDuration(sign, result, nudgeResult.nudgedEpochNs, isoDateTime, timeZone, calendar,
                                        largestUnit, startUnit));
  }
  return result;
}

Result<double> TotalRelativeDuration(const InternalDuration& duration, EpochNanoseconds originEpochNs,
                                     EpochNanoseconds destEpochNs, const ISODateTime& isoDateTime,
                                     const TimeZone* timeZone, const CalendarId& calendar, Unit unit) {
  if (IsCalendarUnit(unit) || (timeZone && unit == Unit::Day)) {
    int sign = InternalDurationSign(duration) < 0 ? -1 : 1;
    CalendarNudge record = TRY(NudgeToCalendarUnit(sign, duration, originEpochNs, destEpochNs, isoDateTime, timeZone,
                                                   calendar, 1, unit, RoundingMode::Trunc));
    return record.total;
  }
  TimeDuration timeDuration = TRY(Add24HourDaysToTimeDuration(duration.time, duration.date.days));
  return TotalTimeDuration(timeDuration, unit);
}

Result<InternalDuration> DifferencePlainDateTimeWithRounding(const ISODateTime& isoDateTime1, const ISODateTime& isoDateTime2,
                                                             const CalendarId& calendar, Unit largestUnit,
                                                             int64_t roundingIncrement, Unit smallestUnit,
                                                             RoundingMode roundingMode) {
  if (CompareISODateTime(isoDateTime1, isoDateTime2) == 0) return CombineDateAndTimeDuration(DateDuration{}, 0);
  if (!ISODateTimeWithinLimits(isoDateTime1) || !ISODateTimeWithinLimits(isoDateTime2))
    return ThrowRangeError("date-time is outside the supported range");
  InternalDuration diff = DifferenceISODateTime(isoDateTime1, isoDateTime2, calendar, largestUnit);
  if (smallestUnit == Unit::Nanosecond && roundingIncrement == 1) return diff;
  EpochNanoseconds originEpochNs = GetUTCEpochNanoseconds(isoDateTime1);
  EpochNanoseconds destEpochNs = GetUTCEpochNanoseconds(isoDateTime2);
  return RoundRelativeDuration(diff, originEpochNs, destEpochNs, isoDateTime1, nullptr, calendar, largestUnit,
                               roundingIncrement, smallestUnit, roundingMode);
}

Result<double> DifferencePlainDateTimeWithTotal(const ISODateTime& isoDateTime1, const ISODateTime& isoDateTime2,
                                                const CalendarId& calendar, Unit unit) {
  if (CompareISODateTime(isoDateTime1, isoDateTime2) == 0) return 0.0;
  if (!ISODateTimeWithinLimits(isoDateTime1) || !ISODateTimeWithinLimits(isoDateTime2))
    return ThrowRangeError("date-time is outside the supported range");
  InternalDuration diff = DifferenceISODateTime(isoDateTime1, isoDateTime2, calendar, unit);
  if (unit == Unit::Nanosecond) return Int128ToDouble(diff.time);
  EpochNanoseconds originEpochNs = GetUTCEpochNanoseconds(isoDateTime1);
  EpochNanoseconds destEpochNs = GetUTCEpochNanoseconds(isoDateTime2);
  return TotalRelativeDuration(diff, originEpochNs, destEpochNs, isoDateTime1, nullptr, calendar, unit);
}

Result<ISODateTime> AddDateTime(const ISODateTime& isoDateTime, const CalendarId& calendar, const InternalDuration& duration,
                                Overflow overflow) {
  TimeRecord timeResult = AddTime(isoDateTime.time, duration.time);
  DateDuration dateDuration = TRY(AdjustDateDurationRecord(duration.date, timeResult.days));
  ISODate addedDate = TRY(CalendarDateAdd(calendar, isoDateTime.isoDate, dateDuration, overflow));
  return CombineISODateAndTimeRecord(addedDate, timeResult);
}

// ---------------------------------------------------------------------------------------------------------
// relativeTo

namespace {

struct RelativeTo {
  PlainDateObject* plain = nullptr;
  ZonedDateTimeObject* zoned = nullptr;
};

// GetTemporalRelativeToOption
Result<RelativeTo> GetTemporalRelativeToOption(Object* options) {
  Value value = TRY(Get(options, PropertyKey(u"relativeTo")));
  if (value.isUndefined()) return RelativeTo{};
  OffsetBehaviour offsetBehaviour = OffsetBehaviour::Option;
  MatchBehaviour matchBehaviour = MatchBehaviour::MatchExactly;
  CalendarId calendar;
  std::optional<TimeZone> timeZone;
  std::optional<std::u16string> offsetString;
  ISODate isoDate;
  std::optional<TimeRecord> time;
  if (value.isObject()) {
    Object* o = value.asObject();
    if (auto* z = o->as<ZonedDateTimeObject>()) return RelativeTo{nullptr, z};
    if (auto* d = o->as<PlainDateObject>()) return RelativeTo{d, nullptr};
    if (auto* dt = o->as<PlainDateTimeObject>()) {
      PlainDateObject* plainDate = TRY(CreateTemporalDate(dt->isoDateTime.isoDate, dt->calendar));
      return RelativeTo{plainDate, nullptr};
    }
    calendar = TRY(GetTemporalCalendarIdentifierWithISODefault(o));
    CalendarFields fields = TRY(PrepareCalendarFields(calendar, o, kFieldsDate, kFieldsTime | kFieldOffset | kFieldTimeZone, 0u));
    ISODateTime result = TRY(InterpretTemporalDateTimeFields(calendar, fields, Overflow::Constrain));
    timeZone = fields.timeZone;
    offsetString = fields.offset;
    if (!offsetString) offsetBehaviour = OffsetBehaviour::Wall;
    isoDate = result.isoDate;
    time = result.time;
  } else {
    if (!value.isString()) return ThrowTypeError("relativeTo must be a Temporal object, a property bag or a string");
    ParsedISODateTime result = TRY(ParseISODateTime(value.asString()->view(), kDateTimeZoned | kDateTime));
    offsetString = result.timeZone.offsetString;
    if (result.timeZone.annotation) {
      timeZone = TRY(ToTemporalTimeZoneIdentifier(Value(newString(*result.timeZone.annotation))));
      if (result.timeZone.z) offsetBehaviour = OffsetBehaviour::Exact;
      else if (!offsetString) offsetBehaviour = OffsetBehaviour::Wall;
      matchBehaviour = MatchBehaviour::MatchMinutes;
      if (offsetString && HasSubMinutePrecision(*offsetString)) matchBehaviour = MatchBehaviour::MatchExactly;
    }
    calendar = TRY(CanonicalizeCalendar(result.calendar ? *result.calendar : std::u16string(u"iso8601")));
    isoDate = CreateISODateRecord(*result.year, result.month, result.day);
    time = result.time;
  }
  if (!timeZone) {
    PlainDateObject* plainDate = TRY(CreateTemporalDate(isoDate, calendar));
    return RelativeTo{plainDate, nullptr};
  }
  int64_t offsetNs = 0;
  if (offsetBehaviour == OffsetBehaviour::Option) offsetNs = TRY(ParseDateTimeUTCOffset(*offsetString));
  EpochNanoseconds epochNanoseconds = TRY(InterpretISODateTimeOffset(isoDate, time, offsetBehaviour, offsetNs, *timeZone,
                                                                     Disambiguation::Compatible, OffsetOption::Reject,
                                                                     matchBehaviour));
  ZonedDateTimeObject* zoned = TRY(CreateTemporalZonedDateTime(epochNanoseconds, *timeZone, calendar));
  return RelativeTo{nullptr, zoned};
}

// DateDurationDays(dateDuration, plainRelativeTo)
Result<int64_t> DateDurationDays(const DateDuration& dateDuration, PlainDateObject* plainRelativeTo) {
  DateDuration yearsMonthsWeeksDuration = TRY(AdjustDateDurationRecord(dateDuration, 0));
  if (DateDurationSign(yearsMonthsWeeksDuration) == 0) return dateDuration.days;
  ISODate later = TRY(CalendarDateAdd(plainRelativeTo->calendar, plainRelativeTo->isoDate, yearsMonthsWeeksDuration, Overflow::Constrain));
  const ISODate& d = plainRelativeTo->isoDate;
  int64_t epochDays1 = ISODateToEpochDays(d.year, d.month - 1, d.day);
  int64_t epochDays2 = ISODateToEpochDays(later.year, later.month - 1, later.day);
  return dateDuration.days + (epochDays2 - epochDays1);
}

// ---------------------------------------------------------------------------------------------------------
// The constructor and prototype

Result<Value> Duration_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Temporal.Duration requires 'new'");
  double v[10] = {};
  for (size_t i = 0; i < 10; i++)
    if (!call.args[i].isUndefined()) v[i] = TRY(ToIntegerIfIntegral(call.args[i]));
  DurationFields f{v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]};
  return Value(TRY(CreateTemporalDuration(f, call.newTarget)));
}

Result<Value> Duration_from(NativeCall& call) { return Value(TRY(ToTemporalDuration(call.args[0]))); }

Result<Value> Duration_compare(NativeCall& call) {
  DurationObject* one = TRY(ToTemporalDuration(call.args[0]));
  DurationObject* two = TRY(ToTemporalDuration(call.args[1]));
  Object* resolvedOptions = TRY(GetOptionsObject(call.args[2]));
  RelativeTo relativeTo = TRY(GetTemporalRelativeToOption(resolvedOptions));
  const DurationFields& a = one->fields;
  const DurationFields& b = two->fields;
  if (a.years == b.years && a.months == b.months && a.weeks == b.weeks && a.days == b.days && a.hours == b.hours &&
      a.minutes == b.minutes && a.seconds == b.seconds && a.milliseconds == b.milliseconds &&
      a.microseconds == b.microseconds && a.nanoseconds == b.nanoseconds)
    return num(0);
  Unit largestUnit1 = DefaultTemporalLargestUnit(a);
  Unit largestUnit2 = DefaultTemporalLargestUnit(b);
  InternalDuration duration1 = ToInternalDurationRecord(a);
  InternalDuration duration2 = ToInternalDurationRecord(b);
  if (relativeTo.zoned && (IsDateUnit(largestUnit1) || IsDateUnit(largestUnit2))) {
    ZonedDateTimeObject* z = relativeTo.zoned;
    EpochNanoseconds after1 = TRY(AddZonedDateTime(z->epochNanoseconds, z->timeZone, z->calendar, duration1, Overflow::Constrain));
    EpochNanoseconds after2 = TRY(AddZonedDateTime(z->epochNanoseconds, z->timeZone, z->calendar, duration2, Overflow::Constrain));
    return num(after1 > after2 ? 1 : after1 < after2 ? -1 : 0);
  }
  int64_t days1, days2;
  if (IsCalendarUnit(largestUnit1) || IsCalendarUnit(largestUnit2)) {
    if (!relativeTo.plain) return ThrowRangeError("relativeTo is required to compare durations with calendar units");
    days1 = TRY(DateDurationDays(duration1.date, relativeTo.plain));
    days2 = TRY(DateDurationDays(duration2.date, relativeTo.plain));
  } else {
    days1 = int64_t(a.days);
    days2 = int64_t(b.days);
  }
  TimeDuration timeDuration1 = TRY(Add24HourDaysToTimeDuration(duration1.time, days1));
  TimeDuration timeDuration2 = TRY(Add24HourDaysToTimeDuration(duration2.time, days2));
  return num(CompareTimeDuration(timeDuration1, timeDuration2));
}

Result<DurationObject*> thisDuration(NativeCall& call, const char* method) {
  return thisTemporal<DurationObject>(call.thisValue, method);
}

#define DURATION_GETTER(field)                                                                   \
  Result<Value> DurationPrototype_##field(NativeCall& call) {                                   \
    DurationObject* d = TRY(thisDuration(call, "Temporal.Duration.prototype." #field));          \
    return num(d->fields.field);                                                                 \
  }
DURATION_GETTER(years)
DURATION_GETTER(months)
DURATION_GETTER(weeks)
DURATION_GETTER(days)
DURATION_GETTER(hours)
DURATION_GETTER(minutes)
DURATION_GETTER(seconds)
DURATION_GETTER(milliseconds)
DURATION_GETTER(microseconds)
DURATION_GETTER(nanoseconds)
#undef DURATION_GETTER

Result<Value> DurationPrototype_sign(NativeCall& call) {
  DurationObject* d = TRY(thisDuration(call, "Temporal.Duration.prototype.sign"));
  return num(DurationSign(d->fields));
}

Result<Value> DurationPrototype_blank(NativeCall& call) {
  DurationObject* d = TRY(thisDuration(call, "Temporal.Duration.prototype.blank"));
  return Value::boolean(DurationSign(d->fields) == 0);
}

Result<Value> DurationPrototype_with(NativeCall& call) {
  DurationObject* d = TRY(thisDuration(call, "Temporal.Duration.prototype.with"));
  PartialDuration p = TRY(ToTemporalPartialDurationRecord(call.args[0]));
  const DurationFields& f = d->fields;
  DurationFields r{p.years.value_or(f.years),
                   p.months.value_or(f.months),
                   p.weeks.value_or(f.weeks),
                   p.days.value_or(f.days),
                   p.hours.value_or(f.hours),
                   p.minutes.value_or(f.minutes),
                   p.seconds.value_or(f.seconds),
                   p.milliseconds.value_or(f.milliseconds),
                   p.microseconds.value_or(f.microseconds),
                   p.nanoseconds.value_or(f.nanoseconds)};
  return Value(TRY(CreateTemporalDuration(r)));
}

Result<Value> DurationPrototype_negated(NativeCall& call) {
  DurationObject* d = TRY(thisDuration(call, "Temporal.Duration.prototype.negated"));
  return Value(CreateNegatedTemporalDuration(d));
}

Result<Value> DurationPrototype_abs(NativeCall& call) {
  DurationObject* d = TRY(thisDuration(call, "Temporal.Duration.prototype.abs"));
  const DurationFields& f = d->fields;
  DurationFields r{std::fabs(f.years),        std::fabs(f.months),       std::fabs(f.weeks),       std::fabs(f.days),
                   std::fabs(f.hours),        std::fabs(f.minutes),      std::fabs(f.seconds),     std::fabs(f.milliseconds),
                   std::fabs(f.microseconds), std::fabs(f.nanoseconds)};
  return Value(TRY(CreateTemporalDuration(r)));
}

// AddDurations(operation, duration, other)
Result<Value> AddDurations(ArithmeticOperation operation, DurationObject* duration, Value otherValue) {
  DurationObject* other = TRY(ToTemporalDuration(otherValue));
  if (operation == ArithmeticOperation::Subtract) other = CreateNegatedTemporalDuration(other);
  Unit largestUnit1 = DefaultTemporalLargestUnit(duration->fields);
  Unit largestUnit2 = DefaultTemporalLargestUnit(other->fields);
  Unit largestUnit = LargerOfTwoTemporalUnits(largestUnit1, largestUnit2);
  if (IsCalendarUnit(largestUnit)) return ThrowRangeError("cannot add durations with calendar units without relativeTo");
  InternalDuration d1 = ToInternalDurationRecordWith24HourDays(duration->fields);
  InternalDuration d2 = ToInternalDurationRecordWith24HourDays(other->fields);
  TimeDuration timeResult = TRY(AddTimeDuration(d1.time, d2.time));
  InternalDuration result = CombineDateAndTimeDuration(DateDuration{}, timeResult);
  return Value(TRY(TemporalDurationFromInternal(result, largestUnit)));
}

Result<Value> DurationPrototype_add(NativeCall& call) {
  DurationObject* d = TRY(thisDuration(call, "Temporal.Duration.prototype.add"));
  return AddDurations(ArithmeticOperation::Add, d, call.args[0]);
}

Result<Value> DurationPrototype_subtract(NativeCall& call) {
  DurationObject* d = TRY(thisDuration(call, "Temporal.Duration.prototype.subtract"));
  return AddDurations(ArithmeticOperation::Subtract, d, call.args[0]);
}

Result<Value> DurationPrototype_round(NativeCall& call) {
  DurationObject* duration = TRY(thisDuration(call, "Temporal.Duration.prototype.round"));
  Object* roundTo = TRY(GetRoundingOptionsArgument(call.args[0], u"smallestUnit", "Temporal.Duration.prototype.round"));
  bool smallestUnitPresent = true, largestUnitPresent = true;
  Unit largestUnit = TRY(GetTemporalUnitValuedOption(roundTo, u"largestUnit"));
  RelativeTo relativeTo = TRY(GetTemporalRelativeToOption(roundTo));
  int64_t roundingIncrement = TRY(GetRoundingIncrementOption(roundTo));
  RoundingMode roundingMode = TRY(GetRoundingModeOption(roundTo, RoundingMode::HalfExpand));
  Unit smallestUnit = TRY(GetTemporalUnitValuedOption(roundTo, u"smallestUnit"));
  TRY(ValidateTemporalUnitValue(smallestUnit, UnitGroup::DateTime));
  if (smallestUnit == Unit::Unset) {
    smallestUnitPresent = false;
    smallestUnit = Unit::Nanosecond;
  }
  Unit existingLargestUnit = DefaultTemporalLargestUnit(duration->fields);
  Unit defaultLargestUnit = LargerOfTwoTemporalUnits(existingLargestUnit, smallestUnit);
  TRY(ValidateTemporalUnitValue(largestUnit, UnitGroup::DateTime, true));
  if (largestUnit == Unit::Unset) {
    largestUnitPresent = false;
    largestUnit = defaultLargestUnit;
  } else if (largestUnit == Unit::Auto) {
    largestUnit = defaultLargestUnit;
  }
  if (!smallestUnitPresent && !largestUnitPresent) return ThrowRangeError("smallestUnit or largestUnit is required");
  if (LargerOfTwoTemporalUnits(largestUnit, smallestUnit) != largestUnit)
    return ThrowRangeError("largestUnit must be larger than smallestUnit");
  int64_t maximum = MaximumTemporalDurationRoundingIncrement(smallestUnit);
  if (maximum) TRY(ValidateTemporalRoundingIncrement(roundingIncrement, maximum, false));
  if (roundingIncrement > 1 && largestUnit != smallestUnit && IsDateUnit(smallestUnit))
    return ThrowRangeError("roundingIncrement > 1 requires largestUnit = smallestUnit for date units");
  if (relativeTo.zoned) {
    ZonedDateTimeObject* z = relativeTo.zoned;
    InternalDuration internalDuration = ToInternalDurationRecord(duration->fields);
    EpochNanoseconds targetEpochNs =
        TRY(AddZonedDateTime(z->epochNanoseconds, z->timeZone, z->calendar, internalDuration, Overflow::Constrain));
    internalDuration = TRY(DifferenceZonedDateTimeWithRounding(z->epochNanoseconds, targetEpochNs, z->timeZone, z->calendar,
                                                               largestUnit, roundingIncrement, smallestUnit, roundingMode));
    if (IsDateUnit(largestUnit)) largestUnit = Unit::Hour;
    return Value(TRY(TemporalDurationFromInternal(internalDuration, largestUnit)));
  }
  if (relativeTo.plain) {
    PlainDateObject* p = relativeTo.plain;
    InternalDuration internalDuration = ToInternalDurationRecordWith24HourDays(duration->fields);
    TimeRecord targetTime = AddTime(MidnightTimeRecord(), internalDuration.time);
    DateDuration dateDuration = TRY(AdjustDateDurationRecord(internalDuration.date, targetTime.days));
    ISODate targetDate = TRY(CalendarDateAdd(p->calendar, p->isoDate, dateDuration, Overflow::Constrain));
    ISODateTime isoDateTime = CombineISODateAndTimeRecord(p->isoDate, MidnightTimeRecord());
    ISODateTime targetDateTime = CombineISODateAndTimeRecord(targetDate, targetTime);
    internalDuration = TRY(DifferencePlainDateTimeWithRounding(isoDateTime, targetDateTime, p->calendar, largestUnit,
                                                               roundingIncrement, smallestUnit, roundingMode));
    return Value(TRY(TemporalDurationFromInternal(internalDuration, largestUnit)));
  }
  if (IsCalendarUnit(existingLargestUnit) || IsCalendarUnit(largestUnit))
    return ThrowRangeError("relativeTo is required to round durations with calendar units");
  InternalDuration internalDuration = ToInternalDurationRecordWith24HourDays(duration->fields);
  if (smallestUnit == Unit::Day) {
    Int128 rounded = RoundNumberToIncrement(internalDuration.time, Int128(nsPerDay) * roundingIncrement, roundingMode);
    DateDuration dateDuration = TRY(CreateDateDurationRecord(0, 0, 0, int64_t(rounded / nsPerDay)));
    internalDuration = CombineDateAndTimeDuration(dateDuration, 0);
  } else {
    TimeDuration timeDuration = TRY(RoundTimeDuration(internalDuration.time, roundingIncrement, smallestUnit, roundingMode));
    internalDuration = CombineDateAndTimeDuration(DateDuration{}, timeDuration);
  }
  return Value(TRY(TemporalDurationFromInternal(internalDuration, largestUnit)));
}

Result<Value> DurationPrototype_total(NativeCall& call) {
  DurationObject* duration = TRY(thisDuration(call, "Temporal.Duration.prototype.total"));
  Object* totalOf = TRY(GetRoundingOptionsArgument(call.args[0], u"unit", "Temporal.Duration.prototype.total"));
  RelativeTo relativeTo = TRY(GetTemporalRelativeToOption(totalOf));
  Unit unit = TRY(GetTemporalUnitValuedOption(totalOf, u"unit", true));
  TRY(ValidateTemporalUnitValue(unit, UnitGroup::DateTime));
  double total;
  if (relativeTo.zoned) {
    ZonedDateTimeObject* z = relativeTo.zoned;
    InternalDuration internalDuration = ToInternalDurationRecord(duration->fields);
    EpochNanoseconds targetEpochNs =
        TRY(AddZonedDateTime(z->epochNanoseconds, z->timeZone, z->calendar, internalDuration, Overflow::Constrain));
    total = TRY(DifferenceZonedDateTimeWithTotal(z->epochNanoseconds, targetEpochNs, z->timeZone, z->calendar, unit));
  } else if (relativeTo.plain) {
    PlainDateObject* p = relativeTo.plain;
    InternalDuration internalDuration = ToInternalDurationRecordWith24HourDays(duration->fields);
    TimeRecord targetTime = AddTime(MidnightTimeRecord(), internalDuration.time);
    DateDuration dateDuration = TRY(AdjustDateDurationRecord(internalDuration.date, targetTime.days));
    ISODate targetDate = TRY(CalendarDateAdd(p->calendar, p->isoDate, dateDuration, Overflow::Constrain));
    ISODateTime isoDateTime = CombineISODateAndTimeRecord(p->isoDate, MidnightTimeRecord());
    ISODateTime targetDateTime = CombineISODateAndTimeRecord(targetDate, targetTime);
    total = TRY(DifferencePlainDateTimeWithTotal(isoDateTime, targetDateTime, p->calendar, unit));
  } else {
    Unit largestUnit = DefaultTemporalLargestUnit(duration->fields);
    if (IsCalendarUnit(largestUnit) || IsCalendarUnit(unit))
      return ThrowRangeError("relativeTo is required to total durations with calendar units");
    InternalDuration internalDuration = ToInternalDurationRecordWith24HourDays(duration->fields);
    total = TotalTimeDuration(internalDuration.time, unit);
  }
  return num(total);
}

Result<Value> DurationPrototype_toString(NativeCall& call) {
  DurationObject* duration = TRY(thisDuration(call, "Temporal.Duration.prototype.toString"));
  Object* resolvedOptions = TRY(GetOptionsObject(call.args[0]));
  int digits = TRY(GetTemporalFractionalSecondDigitsOption(resolvedOptions));
  RoundingMode roundingMode = TRY(GetRoundingModeOption(resolvedOptions, RoundingMode::Trunc));
  Unit smallestUnit = TRY(GetTemporalUnitValuedOption(resolvedOptions, u"smallestUnit"));
  TRY(ValidateTemporalUnitValue(smallestUnit, UnitGroup::Time));
  if (smallestUnit == Unit::Hour || smallestUnit == Unit::Minute)
    return ThrowRangeError("smallestUnit must be seconds or smaller");
  SecondsStringPrecision precision = TRY(ToSecondsStringPrecisionRecord(smallestUnit, digits));
  if (precision.unit == Unit::Nanosecond && precision.increment == 1)
    return Value(newString(TemporalDurationToString(duration->fields, precision.precision)));
  Unit largestUnit = DefaultTemporalLargestUnit(duration->fields);
  InternalDuration internalDuration = ToInternalDurationRecord(duration->fields);
  TimeDuration timeDuration = TRY(RoundTimeDuration(internalDuration.time, precision.increment, precision.unit, roundingMode));
  internalDuration = CombineDateAndTimeDuration(internalDuration.date, timeDuration);
  Unit roundedLargestUnit = LargerOfTwoTemporalUnits(largestUnit, Unit::Second);
  DurationFields rounded = TRY(TemporalDurationFieldsFromInternal(internalDuration, roundedLargestUnit));
  return Value(newString(TemporalDurationToString(rounded, precision.precision)));
}

Result<Value> DurationPrototype_toJSON(NativeCall& call) {
  DurationObject* duration = TRY(thisDuration(call, "Temporal.Duration.prototype.toJSON"));
  return Value(newString(TemporalDurationToString(duration->fields, -1)));
}

Result<Value> DurationPrototype_toLocaleString(NativeCall& call) {
  DurationObject* duration = TRY(thisDuration(call, "Temporal.Duration.prototype.toLocaleString"));
  return Value(newString(TemporalDurationToString(duration->fields, -1)));
}

Result<Value> DurationPrototype_valueOf(NativeCall&) {
  return ThrowTypeError("Temporal.Duration.prototype.valueOf: use compare() to compare durations");
}

void installDuration(Realm& realm) {
  Object* proto = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  BuiltinFunction* ctor = defineConstructor(realm, Intrinsic::TemporalDuration, Duration_construct, 0, PropertyKey(u"Duration"),
                                            proto, Intrinsic::TemporalDurationPrototype, false);
  defineValue(TemporalNamespace(realm), PropertyKey(u"Duration"), Value(ctor));
  defineFunction(realm, ctor, PropertyKey(u"compare"), 2, Duration_compare);
  defineFunction(realm, ctor, PropertyKey(u"from"), 1, Duration_from);

  defineAccessor(realm, proto, PropertyKey(u"years"), DurationPrototype_years, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"months"), DurationPrototype_months, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"weeks"), DurationPrototype_weeks, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"days"), DurationPrototype_days, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"hours"), DurationPrototype_hours, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"minutes"), DurationPrototype_minutes, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"seconds"), DurationPrototype_seconds, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"milliseconds"), DurationPrototype_milliseconds, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"microseconds"), DurationPrototype_microseconds, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"nanoseconds"), DurationPrototype_nanoseconds, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"sign"), DurationPrototype_sign, nullptr);
  defineAccessor(realm, proto, PropertyKey(u"blank"), DurationPrototype_blank, nullptr);
  defineFunction(realm, proto, PropertyKey(u"with"), 1, DurationPrototype_with);
  defineFunction(realm, proto, PropertyKey(u"negated"), 0, DurationPrototype_negated);
  defineFunction(realm, proto, PropertyKey(u"abs"), 0, DurationPrototype_abs);
  defineFunction(realm, proto, PropertyKey(u"add"), 1, DurationPrototype_add);
  defineFunction(realm, proto, PropertyKey(u"subtract"), 1, DurationPrototype_subtract);
  defineFunction(realm, proto, PropertyKey(u"round"), 1, DurationPrototype_round);
  defineFunction(realm, proto, PropertyKey(u"total"), 1, DurationPrototype_total);
  defineFunction(realm, proto, PropertyKey(u"toString"), 0, DurationPrototype_toString);
  defineFunction(realm, proto, PropertyKey(u"toJSON"), 0, DurationPrototype_toJSON);
  defineFunction(realm, proto, PropertyKey(u"toLocaleString"), 0, DurationPrototype_toLocaleString);
  defineFunction(realm, proto, PropertyKey(u"valueOf"), 0, DurationPrototype_valueOf);
  defineValue(proto, PropertyKey(builtins::symbols().toStringTag), builtins::str(u"Temporal.Duration"), Attr::Configurable);
}

}  // namespace

SNOWMAN_BUILTIN(TemporalDuration, kOrderDefault, installDuration);

}  // namespace snowman::temporal
