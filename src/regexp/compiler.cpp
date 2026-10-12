// The RegExp compiler: the AST (ast.h) to the VM program (program.h), following CompilePattern (22.2.2).
// CharSets are evaluated here, per CompileToCharSet and CompileCharacterClass; with IgnoreCase a character
// matcher gets the case closure of its set, so the VM never canonicalizes for a character test.
#include <algorithm>
#include <optional>
#include <set>

#include "regexp/ast.h"
#include "regexp/program.h"

namespace snowman::regexp {
namespace {

uint8_t flagIf(bool condition, uint8_t flag) { return condition ? flag : 0; }

// A CharSet with v: single characters in chars, the other CharSetElements (the empty string, longer strings)
// in strings.
struct ClassValue {
  CharSet chars;
  std::set<std::u32string> strings;
};

const CharSet& digitChars() {
  static const CharSet s = CharSet::range('0', '9');
  return s;
}

// WhiteSpace and LineTerminator (\s).
const CharSet& spaceChars() {
  static const CharSet s = [] {
    CharSet c;
    for (char32_t x : {0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x20, 0xA0, 0x2028, 0x2029, 0xFEFF}) c.add(x);
    auto zs = unicode::lookupLoneProperty("Zs");
    for (const auto& r : unicode::propertyCodePoints(*zs)) c.add(r.first, r.last);
    c.ranges();
    return c;
  }();
  return s;
}

const CharSet& basicWordChars() {
  static const CharSet s = [] {
    CharSet c;
    c.add('0', '9');
    c.add('A', 'Z');
    c.add('_');
    c.add('a', 'z');
    c.ranges();
    return c;
  }();
  return s;
}

// WordCharacters with u or v and i: the basic ones and every character that canonicalizes to one (U+017F, U+212A).
const CharSet& unicodeIgnoreCaseWordChars() {
  static const CharSet s = [] {
    CharSet c = basicWordChars();
    for (const auto& m : unicode::simpleCaseFoldings()) {
      if (basicWordChars().contains(m.to) && !basicWordChars().contains(m.from)) c.add(m.from);
    }
    c.ranges();
    return c;
  }();
  return s;
}

class Compiler {
 public:
  Compiler(const Pattern& pattern, Program& program)
      : pattern_(pattern), p_(program), u_(pattern.flags.unicode || pattern.flags.unicodeSets), v_(pattern.flags.unicodeSets) {}

  void run() {
    p_.flags = pattern_.flags;
    p_.unicodeMode = u_;
    p_.captureCount = pattern_.captureCount;
    p_.groupNames.assign(1, u"");
    for (const std::u16string& name : pattern_.groupNames) {
      p_.groupNames.push_back(name);
      if (!name.empty()) p_.hasGroupNames = true;
    }
    p_.anchoredStart = startsAnchored(pattern_.root);
    compile(pattern_.root, false);
    emit({Op::Match});
  }

 private:
  // ---- the RegExp Record in scope ----
  struct Rer {
    bool ignoreCase;
  };
  static Rer rer(const Node* n) { return {(n->mods & kIgnoreCase) != 0}; }
  CaseMode caseMode() const { return u_ ? CaseMode::Unicode : CaseMode::Legacy; }
  char32_t maxChar() const { return u_ ? kMaxCodePoint : kMaxCodeUnit; }
  // AllCharacters(rer).
  CharSet allCharacters(Rer r) const {
    if (v_ && r.ignoreCase) return simpleCaseFoldFixedPoints();
    return CharSet::range(0, maxChar());
  }
  CharSet characterComplement(Rer r, const CharSet& s) const { return allCharacters(r).subtract(s); }
  const CharSet& wordCharacters(Rer r) const { return u_ && r.ignoreCase ? unicodeIgnoreCaseWordChars() : basicWordChars(); }
  // MaybeSimpleCaseFolding.
  bool folds(Rer r) const { return v_ && r.ignoreCase; }
  CharSet fold(Rer r, const CharSet& s) const { return folds(r) ? canonicalizeSet(CaseMode::Unicode, s) : s; }
  std::u32string fold(Rer r, std::u32string s) const {
    if (folds(r)) {
      for (char32_t& c : s) c = unicode::simpleCaseFold(c);
    }
    return s;
  }

  // ---- emitting ----
  uint32_t emit(Inst inst) {
    p_.code.push_back(inst);
    return static_cast<uint32_t>(p_.code.size() - 1);
  }
  uint32_t here() const { return static_cast<uint32_t>(p_.code.size()); }
  uint32_t registers(uint32_t n) {
    uint32_t r = p_.registerCount;
    p_.registerCount += n;
    return r;
  }
  static uint8_t dir(bool backward) { return backward ? kBackward : 0; }

  // The single-character matcher accepting exactly `accepted` (already case-closed).
  Inst setMatcher(const CharSet& accepted, bool backward) {
    const auto& rs = accepted.ranges();
    if (rs.size() == 1 && rs[0].first == rs[0].last) return {Op::Char, dir(backward), rs[0].first};
    if (rs.size() == 1 && rs[0].first == 0 && rs[0].last >= maxChar()) return {Op::Any, dir(backward)};
    p_.sets.push_back(accepted);
    return {Op::Set, dir(backward), static_cast<uint32_t>(p_.sets.size() - 1)};
  }
  // CharacterSetMatcher(rer, A, invert).
  Inst characterSetMatcher(Rer r, const CharSet& a, bool invert, bool backward) {
    CharSet accepted = r.ignoreCase ? caseClosure(caseMode(), a) : a;
    if (invert) accepted = accepted.complement(maxChar());
    return setMatcher(accepted, backward);
  }
  Inst charMatcher(Rer r, char32_t c, bool backward) {
    if (!r.ignoreCase) return {Op::Char, dir(backward), c};
    return characterSetMatcher(r, CharSet::of(c), false, backward);
  }

  // ---- character classes ----

  // CompileToCharSet of one item of a NonemptyClassRanges (no v flag).
  CharSet itemSet(Rer r, const ClassItem& item) const {
    switch (item.kind) {
      case ClassItem::Kind::Char:
        return CharSet::of(item.lo);
      case ClassItem::Kind::Range:
        return CharSet::range(item.lo, item.hi);
      case ClassItem::Kind::Escape:
        return escapeSet(r, item.escape);
      case ClassItem::Kind::Property: {
        CharSet s = CharSet::fromRanges(unicode::propertyCodePoints(item.property));
        return item.negated ? characterComplement(r, s) : s;
      }
      default:
        return {};
    }
  }

  // CharacterClassEscape :: d D s S w W (folded where the spec folds: only \w and \W).
  CharSet escapeSet(Rer r, char escape) const {
    switch (escape) {
      case 'd':
        return digitChars();
      case 'D':
        return characterComplement(r, digitChars());
      case 's':
        return spaceChars();
      case 'S':
        return characterComplement(r, spaceChars());
      case 'w':
        return fold(r, wordCharacters(r));
      case 'W':
        return characterComplement(r, fold(r, wordCharacters(r)));
    }
    return {};
  }

  void addElement(ClassValue& v, std::u32string s) const {
    if (s.size() == 1) {
      v.chars.add(s[0]);
    } else {
      v.strings.insert(std::move(s));
    }
  }

  // CompileToCharSet with v.
  ClassValue itemValue(Rer r, const ClassItem& item) const {
    ClassValue v;
    switch (item.kind) {
      case ClassItem::Kind::Char:
        v.chars = fold(r, CharSet::of(item.lo));
        break;
      case ClassItem::Kind::Range:
        v.chars = fold(r, CharSet::range(item.lo, item.hi));
        break;
      case ClassItem::Kind::Escape:
        v.chars = escapeSet(r, item.escape);
        break;
      case ClassItem::Kind::Property: {
        CharSet s = CharSet::fromRanges(unicode::propertyCodePoints(item.property));
        // A lone General_Category value is the one property CompileToCharSet returns without
        // MaybeSimpleCaseFolding.
        if (!item.loneGeneralCategory) s = fold(r, s);
        if (item.negated) {
          v.chars = characterComplement(r, s);
        } else {
          v.chars = s;
          for (size_t i = 0; i < unicode::propertyStringCount(item.property); i++) {
            addElement(v, fold(r, std::u32string(unicode::propertyString(item.property, i))));
          }
        }
        break;
      }
      case ClassItem::Kind::Nested:
        v = classValue(r, *item.nested);
        break;
      case ClassItem::Kind::Strings:
        for (const std::u32string& s : item.strings) addElement(v, fold(r, s));
        break;
    }
    v.chars.ranges();
    return v;
  }

  // A ClassContents with v, negation included (NestedClass / CharacterClass [^...]: CharacterComplement).
  ClassValue classValue(Rer r, const ClassNode& cls) const {
    ClassValue v;
    bool first = true;
    for (const ClassItem& item : cls.items) {
      ClassValue x = itemValue(r, item);
      if (first) {
        v = std::move(x);
        first = false;
        continue;
      }
      switch (cls.op) {
        case ClassNode::Op::Union:
          v.chars = v.chars.unionWith(x.chars);
          v.strings.insert(x.strings.begin(), x.strings.end());
          break;
        case ClassNode::Op::Intersection: {
          v.chars = v.chars.intersect(x.chars);
          std::set<std::u32string> both;
          for (const auto& s : v.strings) {
            if (x.strings.count(s)) both.insert(s);
          }
          v.strings = std::move(both);
          break;
        }
        case ClassNode::Op::Subtraction:
          v.chars = v.chars.subtract(x.chars);
          for (const auto& s : x.strings) v.strings.erase(s);
          break;
      }
    }
    if (cls.negated) v.chars = characterComplement(r, v.chars);
    return v;
  }

  void compileClass(const Node* n, bool backward) {
    Rer r = rer(n);
    if (!v_) {
      CharSet a;
      for (const ClassItem& item : n->cls->items) a.addAll(itemSet(r, item));
      emit(characterSetMatcher(r, a, n->cls->negated, backward));
      return;
    }
    ClassValue v = classValue(r, *n->cls);
    if (v.strings.empty()) {
      emit(characterSetMatcher(r, v.chars, false, backward));
      return;
    }
    // CompileAtom :: CharacterClass with strings: the strings longest first, then the single characters, then the
    // empty string, as alternatives.
    std::vector<std::u32string> strings(v.strings.begin(), v.strings.end());
    std::stable_sort(strings.begin(), strings.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    bool hasEmpty = !strings.empty() && strings.back().empty();
    if (hasEmpty) strings.pop_back();
    size_t alternatives = strings.size() + (v.chars.empty() ? 0 : 1) + (hasEmpty ? 1 : 0);
    std::vector<uint32_t> jumps;
    size_t k = 0;
    auto alternative = [&](auto&& body) {
      bool last = ++k == alternatives;
      uint32_t split = last ? 0 : emit({Op::Split});
      body();
      if (!last) {
        jumps.push_back(emit({Op::Jump}));
        p_.code[split].a = here();
      }
    };
    for (const std::u32string& s : strings) {
      alternative([&] {
        if (backward) {
          for (auto it = s.rbegin(); it != s.rend(); ++it) emit(charMatcher(r, *it, true));
        } else {
          for (char32_t c : s) emit(charMatcher(r, c, false));
        }
      });
    }
    if (!v.chars.empty()) alternative([&] { emit(characterSetMatcher(r, v.chars, false, backward)); });
    if (hasEmpty) alternative([] {});
    for (uint32_t j : jumps) p_.code[j].a = here();
  }

  // ---- terms ----

  // The one instruction a single-character atom compiles to, if it is one (for Star).
  std::optional<Inst> singleCharacter(const Node* n, bool backward) {
    while ((n->kind == Node::Kind::NonCapturing || n->kind == Node::Kind::Sequence) && n->children.size() == 1) {
      n = n->children[0];
    }
    switch (n->kind) {
      case Node::Kind::Char:
        return charMatcher(rer(n), n->value, backward);
      case Node::Kind::Dot:
        return Inst{n->mods & kDotAll ? Op::Any : Op::Dot, dir(backward)};
      case Node::Kind::Class: {
        size_t before = p_.code.size();
        compileClass(n, backward);
        if (p_.code.size() == before + 1) {
          Inst inst = p_.code.back();
          p_.code.pop_back();
          return inst;
        }
        p_.code.resize(before);
        return std::nullopt;
      }
      default:
        return std::nullopt;
    }
  }

  void compile(const Node* n, bool backward) {
    switch (n->kind) {
      case Node::Kind::Empty:
        break;
      case Node::Kind::Char:
        emit(charMatcher(rer(n), n->value, backward));
        break;
      case Node::Kind::Dot:
        emit({n->mods & kDotAll ? Op::Any : Op::Dot, dir(backward)});
        break;
      case Node::Kind::Class:
        compileClass(n, backward);
        break;
      case Node::Kind::Sequence:
        // MatchSequence: right to left inside a lookbehind.
        if (backward) {
          for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) compile(*it, true);
        } else {
          for (const Node* child : n->children) compile(child, false);
        }
        break;
      case Node::Kind::Disjunction: {
        std::vector<uint32_t> jumps;
        for (size_t i = 0; i < n->children.size(); i++) {
          bool last = i + 1 == n->children.size();
          uint32_t split = last ? 0 : emit({Op::Split});
          compile(n->children[i], backward);
          if (!last) {
            jumps.push_back(emit({Op::Jump}));
            p_.code[split].a = here();
          }
        }
        for (uint32_t j : jumps) p_.code[j].a = here();
        break;
      }
      case Node::Kind::Group: {
        uint32_t reg = registers(1);
        emit({Op::GroupStart, 0, reg});
        compile(n->children[0], backward);
        emit({Op::GroupEnd, dir(backward), n->index, reg});
        break;
      }
      case Node::Kind::NonCapturing:
        compile(n->children[0], backward);
        break;
      case Node::Kind::Lookahead:
      case Node::Kind::Lookbehind: {
        uint32_t reg = registers(2);
        uint32_t start = emit({Op::LookStart, flagIf(n->negate, kNegative), reg, 0, n->firstCapture, n->lastCapture});
        compile(n->children[0], n->kind == Node::Kind::Lookbehind);
        uint32_t end = emit({Op::LookEnd, 0, reg, start});
        p_.code[start].b = end;
        break;
      }
      case Node::Kind::Quantifier:
        compileQuantifier(n, backward);
        break;
      case Node::Kind::Backreference:
      case Node::Kind::NamedBackreference: {
        uint32_t first = static_cast<uint32_t>(p_.backrefGroups.size());
        if (n->kind == Node::Kind::Backreference) {
          p_.backrefGroups.push_back(n->index);
        } else {
          for (uint32_t i = 1; i <= p_.captureCount; i++) {
            if (p_.groupNames[i] == n->name) p_.backrefGroups.push_back(i);
          }
        }
        uint8_t flags = dir(backward) | flagIf(n->mods & kIgnoreCase, kInstIgnoreCase);
        emit({Op::Backref, flags, first, static_cast<uint32_t>(p_.backrefGroups.size()) - first});
        break;
      }
      case Node::Kind::LineStart:
      case Node::Kind::LineEnd:
        emit({n->kind == Node::Kind::LineStart ? Op::LineStart : Op::LineEnd, flagIf(n->mods & kMultiline, kInstMultiline)});
        break;
      case Node::Kind::WordBoundary:
      case Node::Kind::NotWordBoundary:
        emit({n->kind == Node::Kind::WordBoundary ? Op::WordBoundary : Op::NotWordBoundary,
              flagIf(u_ && (n->mods & kIgnoreCase), kUnicodeWords)});
        break;
    }
  }

  void compileQuantifier(const Node* n, bool backward) {
    const Node* atom = n->children[0];
    if (n->max == 0) return;  // RepeatMatcher step 1: the continuation, nothing else
    if (n->min == 1 && n->max == 1) {
      // One mandatory iteration: no empty check, and the atom's captures are undefined on entry.
      compile(atom, backward);
      return;
    }
    uint8_t greedy = flagIf(n->greedy, kGreedy);
    if (n->firstCapture == n->lastCapture) {
      if (auto single = singleCharacter(atom, backward)) {
        emit({Op::Star, greedy, 0, n->min, n->max});
        emit(*single);
        return;
      }
    }
    uint32_t reg = registers(2);
    emit({Op::LoopInit, 0, reg});
    uint32_t loop = emit({Op::Loop, greedy, reg, n->min, n->max});
    emit({Op::LoopIter, 0, reg});
    if (n->firstCapture < n->lastCapture) emit({Op::ClearCaptures, 0, n->firstCapture, n->lastCapture});
    compile(atom, backward);
    emit({Op::LoopNext, 0, reg, n->min, loop});
    p_.code[loop].d = here();
  }

  // Only index 0 can match: every alternative starts with ^ and m is off there.
  static bool startsAnchored(const Node* n) {
    switch (n->kind) {
      case Node::Kind::LineStart:
        return !(n->mods & kMultiline);
      case Node::Kind::Sequence:
        return !n->children.empty() && startsAnchored(n->children[0]);
      case Node::Kind::Disjunction:
        return std::all_of(n->children.begin(), n->children.end(), [](const Node* c) { return startsAnchored(c); });
      case Node::Kind::Group:
      case Node::Kind::NonCapturing:
        return startsAnchored(n->children[0]);
      default:
        return false;
    }
  }

  const Pattern& pattern_;
  Program& p_;
  const bool u_;
  const bool v_;
};

}  // namespace

std::shared_ptr<const Program> compile(std::u16string_view pattern, std::u16string_view flags, std::string* error) {
  auto parsed = parseFlags(flags);
  if (!parsed) {
    *error = "Invalid regular expression flags";
    return nullptr;
  }
  auto ast = parsePattern(pattern, *parsed, error);
  if (!ast) return nullptr;
  auto program = std::make_shared<Program>();
  Compiler(*ast, *program).run();
  return program;
}

const Flags& flagsOf(const Program& program) { return program.flags; }
uint32_t captureCount(const Program& program) { return program.captureCount; }
const std::u16string& groupName(const Program& program, uint32_t i) { return program.groupNames[i]; }
bool hasGroupNames(const Program& program) { return program.hasGroupNames; }

}  // namespace snowman::regexp
