// The japanese calendar: ISO months and days; the arithmetic year is the ISO year. Eras from CLDR: reiwa, heisei,
// showa and taisho start mid-year; meiji is an era only from 1873-01-01 (its year 6), and dates before that use
// ce / bce like gregory (proposal-intl-era-monthcode).
#include "builtins/temporal/calendar_isolike.h"

namespace snowman::temporal {

namespace {

const EraRow kJapaneseEras[] = {
    {u"reiwa", nullptr, EraRow::Kind::Offset, 2019},
    {u"heisei", nullptr, EraRow::Kind::Offset, 1989},
    {u"showa", nullptr, EraRow::Kind::Offset, 1926},
    {u"taisho", nullptr, EraRow::Kind::Offset, 1912},
    {u"meiji", nullptr, EraRow::Kind::Offset, 1868},
    {u"ce", u"ad", EraRow::Kind::Epoch, 0},
    {u"bce", u"bc", EraRow::Kind::Negative, 0},
};

struct EraStart {
  int32_t row;  // index into kJapaneseEras
  int32_t year, month, day;
};
// The first ISO day of each offset era, newest first.
const EraStart kEraStarts[] = {
    {0, 2019, 5, 1}, {1, 1989, 1, 8}, {2, 1926, 12, 25}, {3, 1912, 7, 30}, {4, 1873, 1, 1},
};

class JapaneseCalendar final : public IsoLikeCalendar {
 public:
  JapaneseCalendar() : IsoLikeCalendar(0, kJapaneseEras) {}

  std::pair<const EraRow*, int64_t> eraOf(int64_t epochDays, const Date& date) const override {
    for (const EraStart& start : kEraStarts) {
      if (epochDays >= ISODateToEpochDays(start.year, start.month - 1, start.day)) {
        const EraRow& row = kJapaneseEras[start.row];
        return {&row, date.year - row.offset + 1};
      }
    }
    if (date.year >= 1) return {&kJapaneseEras[5], date.year};
    return {&kJapaneseEras[6], 1 - date.year};
  }
  bool hasMidYearEras() const override { return true; }
};

const JapaneseCalendar kJapanese;

}  // namespace

const CalendarSystem* JapaneseCalendarSystem() { return &kJapanese; }

}  // namespace snowman::temporal
