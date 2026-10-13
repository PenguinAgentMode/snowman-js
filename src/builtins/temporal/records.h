// Temporal's plain value records (ISO Date Record, Time Record, ISO Date-Time Record) and the epoch
// nanoseconds type. No dependencies: shared by temporal.h and the time zone layer (time_zone.h).
#pragma once

#include <cstdint>

namespace snowman::temporal {

// Epoch nanoseconds and time durations (nanoseconds): exact integers, |x| < 2^127.
using EpochNanoseconds = __int128;

// ISO Date Record. A stored record is always within ISODateWithinLimits (years -271821..275760).
struct ISODate {
  int32_t year = 1970;
  int32_t month = 1;  // 1..12
  int32_t day = 1;    // 1..ISODaysInMonth
};

// Time Record. days carries the overflow of BalanceTime/AddTime (0 for a stored time of day).
struct TimeRecord {
  int64_t days = 0;
  int32_t hour = 0, minute = 0, second = 0, millisecond = 0, microsecond = 0, nanosecond = 0;
};

// ISO Date-Time Record.
struct ISODateTime {
  ISODate isoDate;
  TimeRecord time;
};

}  // namespace snowman::temporal
