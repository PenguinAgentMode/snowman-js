// Intl.Segmenter (ECMA-402 19): UAX #29 grapheme cluster, word and sentence boundaries over the Unicode 17.0 break
// properties of segmenter_data.inc (no dictionary segmentation, no GB9c conjunct rule), the Segments object and
// the segment iterator.
#include <algorithm>
#include <cmath>

#include "builtins/intl/intl.h"
#include "builtins/string_util.h"
#include "unicode/unicode.h"

namespace snowman::intl {

namespace {

struct BreakRange {
  char32_t first, last;
  uint8_t value;
};
#include "builtins/intl/segmenter_data.inc"

template <size_t N>
uint8_t property(const BreakRange (&table)[N], char32_t cp) {
  const BreakRange* end = table + N;
  const BreakRange* it = std::upper_bound(table, end, cp, [](char32_t c, const BreakRange& r) { return c < r.first; });
  return it != table && cp <= (it - 1)->last ? (it - 1)->value : 0;
}

bool isExtPict(char32_t cp) { return property(kExtendedPictographic, cp) != 0; }

enum GB : uint8_t { GOther, GCR, GLF, GControl, GExtend, GZWJ, GRI, GPrepend, GSpacingMark, GL, GV, GT, GLV, GLVT };
enum WB : uint8_t {
  WOther, WCR, WLF, WNewline, WExtend, WZWJ, WRI, WFormat, WKatakana, WHebrew, WALetter, WSingleQuote, WDoubleQuote,
  WMidNumLet, WMidLetter, WMidNum, WNumeric, WExtendNumLet, WWSegSpace
};
enum SB : uint8_t { SOther, SCR, SLF, SExtend, SSep, SFormat, SSp, SLower, SUpper, SOLetter, SNumeric, SATerm, SSContinue, SSTerm, SClose };

struct CodePoints {
  std::vector<char32_t> cps;
  std::vector<size_t> offsets;  // code unit offset of each code point, plus the length
};

CodePoints decode(std::u16string_view s) {
  CodePoints out;
  for (size_t i = 0; i < s.size();) {
    builtins::CodePointRecord r = builtins::CodePointAt(s, i);
    out.cps.push_back(r.codePoint);
    out.offsets.push_back(i);
    i += size_t(r.codeUnitCount);
  }
  out.offsets.push_back(s.size());
  return out;
}

// Boundaries as code point indices, 0 and n included.
std::vector<size_t> graphemeBoundaries(const std::vector<char32_t>& cps) {
  size_t n = cps.size();
  std::vector<size_t> out{0};
  std::vector<uint8_t> p(n);
  for (size_t i = 0; i < n; i++) p[i] = property(kGraphemeBreak, cps[i]);
  size_t riCount = 0;  // regional indicators before i in the current run
  for (size_t i = 1; i < n; i++) {
    uint8_t a = p[i - 1], b = p[i];
    riCount = a == GRI ? riCount + 1 : 0;
    bool brk;
    if (a == GCR && b == GLF) brk = false;
    else if (a == GControl || a == GCR || a == GLF) brk = true;
    else if (b == GControl || b == GCR || b == GLF) brk = true;
    else if (a == GL && (b == GL || b == GV || b == GLV || b == GLVT)) brk = false;
    else if ((a == GLV || a == GV) && (b == GV || b == GT)) brk = false;
    else if ((a == GLVT || a == GT) && b == GT) brk = false;
    else if (b == GExtend || b == GZWJ || b == GSpacingMark) brk = false;
    else if (a == GPrepend) brk = false;
    else if (a == GZWJ && isExtPict(cps[i])) {
      // GB11: ExtPict Extend* ZWJ × ExtPict
      size_t j = i - 1;
      while (j > 0 && p[j - 1] == GExtend) j--;
      brk = !(j > 0 && isExtPict(cps[j - 1]));
    } else if (a == GRI && b == GRI) {
      brk = riCount % 2 == 0;
    } else {
      brk = true;
    }
    if (brk) out.push_back(i);
  }
  if (n) out.push_back(n);
  return out;
}

bool ahLetter(uint8_t w) { return w == WALetter || w == WHebrew; }
bool midNumLetQ(uint8_t w) { return w == WMidNumLet || w == WSingleQuote; }
bool ignorableWB(uint8_t w) { return w == WExtend || w == WFormat || w == WZWJ; }

std::vector<size_t> wordBoundaries(const std::vector<char32_t>& cps) {
  size_t n = cps.size();
  std::vector<size_t> out{0};
  std::vector<uint8_t> p(n);
  for (size_t i = 0; i < n; i++) p[i] = property(kWordBreak, cps[i]);
  // The previous / next code point index skipping WB4 ignorables (not across hard breaks).
  auto prevIndex = [&](size_t i) -> long {  // last non-ignorable before i
    long j = long(i) - 1;
    while (j >= 0 && ignorableWB(p[size_t(j)])) j--;
    return j;
  };
  auto nextIndex = [&](size_t i) -> size_t {  // first non-ignorable after i
    size_t j = i + 1;
    while (j < n && ignorableWB(p[j])) j++;
    return j;
  };
  for (size_t i = 1; i < n; i++) {
    uint8_t a = p[i - 1], b = p[i];
    bool brk;
    if (a == WCR && b == WLF) brk = false;
    else if (a == WNewline || a == WCR || a == WLF) brk = true;
    else if (b == WNewline || b == WCR || b == WLF) brk = true;
    else if (a == WZWJ && isExtPict(cps[i])) brk = false;
    else if (a == WWSegSpace && b == WWSegSpace) brk = false;
    else if (ignorableWB(b)) brk = false;
    else {
      long ai = prevIndex(i);
      uint8_t x = ai >= 0 ? p[size_t(ai)] : uint8_t(WOther);
      long bi = ai >= 0 ? prevIndex(size_t(ai)) : -1;
      uint8_t w = bi >= 0 ? p[size_t(bi)] : uint8_t(WOther);  // the one before x
      size_t ci = nextIndex(i);
      uint8_t c = ci < n ? p[ci] : uint8_t(WOther);  // the one after b
      if (ai < 0) brk = true;
      else if (ahLetter(x) && ahLetter(b)) brk = false;
      else if (ahLetter(x) && (b == WMidLetter || midNumLetQ(b)) && ahLetter(c)) brk = false;
      else if (ahLetter(w) && (x == WMidLetter || midNumLetQ(x)) && ahLetter(b)) brk = false;
      else if (x == WHebrew && b == WSingleQuote) brk = false;
      else if (x == WHebrew && b == WDoubleQuote && c == WHebrew) brk = false;
      else if (w == WHebrew && x == WDoubleQuote && b == WHebrew) brk = false;
      else if (x == WNumeric && b == WNumeric) brk = false;
      else if (ahLetter(x) && b == WNumeric) brk = false;
      else if (x == WNumeric && ahLetter(b)) brk = false;
      else if (w == WNumeric && (x == WMidNum || midNumLetQ(x)) && b == WNumeric) brk = false;
      else if (x == WNumeric && (b == WMidNum || midNumLetQ(b)) && c == WNumeric) brk = false;
      else if (x == WKatakana && b == WKatakana) brk = false;
      else if ((ahLetter(x) || x == WNumeric || x == WKatakana || x == WExtendNumLet) && b == WExtendNumLet) brk = false;
      else if (x == WExtendNumLet && (ahLetter(b) || b == WNumeric || b == WKatakana)) brk = false;
      else if (x == WRI && b == WRI) {
        size_t count = 0;
        for (long j = ai; j >= 0; j--) {
          if (ignorableWB(p[size_t(j)])) continue;
          if (p[size_t(j)] != WRI) break;
          count++;
        }
        brk = count % 2 == 0;
      } else {
        brk = true;
      }
    }
    if (brk) out.push_back(i);
  }
  if (n) out.push_back(n);
  return out;
}

bool paraSep(uint8_t s) { return s == SSep || s == SCR || s == SLF; }
bool saTerm(uint8_t s) { return s == SSTerm || s == SATerm; }

std::vector<size_t> sentenceBoundaries(const std::vector<char32_t>& cps) {
  size_t n = cps.size();
  std::vector<size_t> out{0};
  // SB5: Extend and Format attach to the preceding character (except after a paragraph separator).
  std::vector<uint8_t> raw(n);
  for (size_t i = 0; i < n; i++) raw[i] = property(kSentenceBreak, cps[i]);
  std::vector<size_t> idx;  // indices of the non-ignored code points
  for (size_t i = 0; i < n; i++) {
    if ((raw[i] == SExtend || raw[i] == SFormat) && !idx.empty() && !paraSep(raw[idx.back()])) continue;
    idx.push_back(i);
  }
  size_t m = idx.size();
  auto at = [&](long k) -> uint8_t { return k >= 0 && size_t(k) < m ? raw[idx[size_t(k)]] : uint8_t(SOther); };
  for (size_t k = 1; k < m; k++) {
    uint8_t a = at(long(k) - 1), b = at(long(k));
    bool brk;
    if (a == SCR && b == SLF) brk = false;
    else if (paraSep(a)) brk = true;
    else {
      // Find the shape "SATerm Close* Sp* ParaSep?" ending just before k.
      long j = long(k) - 1;
      bool sawSp = false;
      while (j >= 0 && at(j) == SSp) j--, sawSp = true;
      while (j >= 0 && at(j) == SClose) j--;
      uint8_t term = at(j);
      if (a == SATerm && b == SNumeric) brk = false;
      else if (a == SATerm && b == SUpper && (at(long(k) - 2) == SUpper || at(long(k) - 2) == SLower)) brk = false;
      else if (saTerm(term)) {
        if (term == SATerm) {
          // SB8: ATerm Close* Sp* × ( ¬(OLetter | Upper | Lower | ParaSep | SATerm) )* Lower
          size_t q = k;
          while (q < m && !(at(long(q)) == SOLetter || at(long(q)) == SUpper || at(long(q)) == SLower || paraSep(at(long(q))) || saTerm(at(long(q))))) q++;
          if (q < m && at(long(q)) == SLower) continue;
        }
        if (b == SSContinue || saTerm(b)) brk = false;                   // SB8a
        else if (!sawSp && (b == SClose || b == SSp || paraSep(b))) brk = false;  // SB9
        else if (b == SSp || paraSep(b)) brk = false;                    // SB10
        else brk = true;                                                  // SB11
      } else {
        brk = false;  // SB998
      }
    }
    if (brk) out.push_back(idx[k]);
  }
  if (n) out.push_back(n);
  return out;
}

bool isWordLikeSegment(const std::vector<char32_t>& cps, size_t from, size_t to) {
  static std::optional<unicode::Property> letters = unicode::lookupLoneProperty("L"), numbers = unicode::lookupLoneProperty("N");
  auto in = [](const std::optional<unicode::Property>& p, char32_t cp) {
    std::span<const unicode::CodePointRange> r = unicode::propertyCodePoints(*p);
    auto it = std::upper_bound(r.begin(), r.end(), cp, [](char32_t c, const unicode::CodePointRange& x) { return c < x.first; });
    return it != r.begin() && cp <= (it - 1)->last;
  };
  for (size_t i = from; i < to; i++) {
    uint8_t w = property(kWordBreak, cps[i]);
    if (ahLetter(w) || w == WNumeric || w == WKatakana || w == WExtendNumLet) return true;
    if (in(letters, cps[i]) || in(numbers, cps[i])) return true;
  }
  return false;
}

class SegmenterObject : public Object {
 public:
  using Object::Object;
  std::string locale, granularity;
};

// %SegmentsPrototype% instances: [[SegmentsSegmenter]], [[SegmentsString]], and the boundaries (code unit offsets).
class SegmentsObject : public Object {
 public:
  SegmentsObject(Object* prototype, String* string, std::string granularity) : Object(prototype), string(string), granularity(std::move(granularity)) {}
  String* string;
  std::string granularity;
  std::vector<size_t> boundaries;  // code unit offsets, 0 and length included
  std::vector<uint8_t> wordLike;   // per segment (word granularity)
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(string);
  }
  void compute() {
    CodePoints cp = decode(string->view());
    std::vector<size_t> b = granularity == "grapheme" ? graphemeBoundaries(cp.cps)
                            : granularity == "word"   ? wordBoundaries(cp.cps)
                                                      : sentenceBoundaries(cp.cps);
    if (b.size() == 1 && cp.cps.empty()) b = {0};
    for (size_t i : b) boundaries.push_back(cp.offsets[i]);
    if (granularity == "word")
      for (size_t i = 0; i + 1 < b.size(); i++) wordLike.push_back(isWordLikeSegment(cp.cps, b[i], b[i + 1]));
  }
};

class SegmentIteratorObject : public Object {
 public:
  SegmentIteratorObject(Object* prototype, SegmentsObject* segments) : Object(prototype), segments(segments) {}
  SegmentsObject* segments;
  size_t position = 0;  // [[IteratedStringNextSegmentCodeUnitIndex]]
  void trace(Tracer& tracer) override {
    Object::trace(tracer);
    tracer.visit(segments);
  }
};

std::vector<std::string> noKeys(const std::string&, std::string_view) { return {}; }
const ServiceData kSegmenterData{data::locales, noKeys};

// 19.1.1 Intl.Segmenter ( [ locales [ , options ] ] )
Result<Value> Segmenter_construct(NativeCall& call) {
  if (!call.newTarget) return ThrowTypeError("Constructor Intl.Segmenter requires 'new'");
  SegmenterObject* s = TRY(OrdinaryCreateFromConstructor<SegmenterObject>(call.newTarget, Intrinsic::IntlSegmenterPrototype));
  std::vector<std::string> requested = TRY(CanonicalizeLocaleList(call.args[0]));
  Object* options = TRY(GetOptionsObject(call.args[1]));
  LocaleOptions opt;
  opt.localeMatcher = TRY(GetLocaleMatcher(options));
  ResolvedLocale r = ResolveLocale(kSegmenterData, requested, opt, {});
  s->locale = r.locale;
  s->granularity = *TRY(GetStringOption(options, u"granularity", {"grapheme", "word", "sentence"}, "grapheme"));
  return Value(s);
}

Result<Value> Segmenter_supportedLocalesOf(NativeCall& call) { return supportedLocalesOf(kSegmenterData, call); }

Result<Value> SegmenterPrototype_resolvedOptions(NativeCall& call) {
  SegmenterObject* s = TRY(thisIntlObject<SegmenterObject>(call.thisValue, "Intl.Segmenter.prototype.resolvedOptions"));
  Object* o = builtins::newPlainObject();
  addProperty(o, u"locale", jsString(s->locale));
  addProperty(o, u"granularity", jsString(s->granularity));
  return Value(o);
}

// 19.3.3 Intl.Segmenter.prototype.segment ( string )
Result<Value> SegmenterPrototype_segment(NativeCall& call) {
  SegmenterObject* s = TRY(thisIntlObject<SegmenterObject>(call.thisValue, "Intl.Segmenter.prototype.segment"));
  String* string = TRY(ToString(call.args[0]));
  SegmentsObject* segments = make<SegmentsObject>(builtins::intrinsic(Intrinsic::IntlSegmentsPrototype), string, s->granularity);
  segments->compute();
  return Value(segments);
}

// 19.7.1 CreateSegmentDataObject ( segmenter, string, startIndex, endIndex )
Object* segmentData(const SegmentsObject& s, size_t segment) {
  size_t start = s.boundaries[segment], end = s.boundaries[segment + 1];
  Object* o = builtins::newPlainObject();
  addProperty(o, u"segment", Value(newString(s.string->view().substr(start, end - start))));
  addProperty(o, u"index", num(double(start)));
  addProperty(o, u"input", Value(s.string));
  if (s.granularity == "word") addProperty(o, u"isWordLike", boolean(s.wordLike[segment]));
  return o;
}

// The segment containing code unit n.
size_t segmentAt(const SegmentsObject& s, size_t n) {
  auto it = std::upper_bound(s.boundaries.begin(), s.boundaries.end(), n);
  return size_t(it - s.boundaries.begin()) - 1;
}

// 19.5.2.1 %SegmentsPrototype%.containing ( index )
Result<Value> SegmentsPrototype_containing(NativeCall& call) {
  SegmentsObject* s = TRY(thisIntlObject<SegmentsObject>(call.thisValue, "%Segments.prototype%.containing"));
  double n = TRY(ToIntegerOrInfinity(call.args[0]));
  if (n < 0 || n >= double(s->string->length())) return Value::undefined();
  return Value(segmentData(*s, segmentAt(*s, size_t(n))));
}

// 19.5.2.2 %SegmentsPrototype% [ @@iterator ] ( )
Result<Value> SegmentsPrototype_iterator(NativeCall& call) {
  SegmentsObject* s = TRY(thisIntlObject<SegmentsObject>(call.thisValue, "%Segments.prototype%[@@iterator]"));
  return Value(make<SegmentIteratorObject>(builtins::intrinsic(Intrinsic::IntlSegmentIteratorPrototype), s));
}

// 19.6.2.1 %SegmentIteratorPrototype%.next ( )
Result<Value> SegmentIteratorPrototype_next(NativeCall& call) {
  SegmentIteratorObject* it = TRY(thisIntlObject<SegmentIteratorObject>(call.thisValue, "%SegmentIterator.prototype%.next"));
  const SegmentsObject& s = *it->segments;
  if (it->position >= s.string->length()) return Value(CreateIteratorResultObject(Value::undefined(), true));
  size_t segment = segmentAt(s, it->position);
  Object* data = segmentData(s, segment);
  it->position = s.boundaries[segment + 1];
  return Value(CreateIteratorResultObject(Value(data), false));
}

}  // namespace

void installSegmenter(Realm& realm, Object* intl) {
  Object* proto = nullptr;
  BuiltinFunction* ctor = defineIntlConstructor(realm, intl, Intrinsic::IntlSegmenter, Segmenter_construct, 0, u"Segmenter",
                                                Intrinsic::IntlSegmenterPrototype, &proto);
  defineFunction(realm, ctor, PropertyKey(u"supportedLocalesOf"), 1, Segmenter_supportedLocalesOf);
  defineFunction(realm, proto, PropertyKey(u"resolvedOptions"), 0, SegmenterPrototype_resolvedOptions);
  defineFunction(realm, proto, PropertyKey(u"segment"), 1, SegmenterPrototype_segment);

  Object* segments = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::ObjectPrototype));
  realm.setIntrinsic(Intrinsic::IntlSegmentsPrototype, segments);
  defineFunction(realm, segments, PropertyKey(u"containing"), 1, SegmentsPrototype_containing);
  defineFunction(realm, segments, PropertyKey(vm().symbols.iterator), 0, SegmentsPrototype_iterator);

  Object* iterator = OrdinaryObjectCreate(realm.intrinsic(Intrinsic::IteratorPrototype));
  realm.setIntrinsic(Intrinsic::IntlSegmentIteratorPrototype, iterator);
  defineFunction(realm, iterator, PropertyKey(u"next"), 0, SegmentIteratorPrototype_next);
  defineValue(iterator, PropertyKey(vm().symbols.toStringTag), str(u"Segmenter String Iterator"), Attr::Configurable);
}

}  // namespace snowman::intl
