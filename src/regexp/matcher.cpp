// The backtracking VM that runs a Program (program.h), and exec: RegExpBuiltinExec's matching loop.
//
// Choice points and undo records share one explicit stack, so neither the input length nor the pattern's
// nesting depth uses the C stack. Backtracking pops the stack: an undo record restores a capture or a register,
// a choice point resumes there. A lookaround leaves a barrier: when its body matches, a positive one cuts the
// stack back to the barrier (no backtracking into it, 22.2.2.4) and a negative one unwinds to it and fails; when
// the body fails, popping the barrier makes a negative lookaround succeed.
#include <algorithm>

#include "regexp/program.h"

namespace snowman::regexp {
namespace {

constexpr size_t kStackLimit = size_t{1} << 23;  // backtrack entries (16 bytes each)

bool isLead(char32_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool isTrail(char32_t c) { return c >= 0xDC00 && c <= 0xDFFF; }
bool isLineTerminator(char32_t c) { return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029; }
bool isWordUnit(char16_t c, bool unicodeIgnoreCase) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') return true;
  return unicodeIgnoreCase && (c == 0x017F || c == 0x212A);
}

enum class Entry : uint8_t { Choice, UndoCapture, UndoRegister, Barrier, StarGreedy, StarLazy };
struct Frame {
  Entry kind;
  uint32_t pc;  // or the capture / register index
  int32_t a;
  int32_t b;
};

class VM {
 public:
  VM(const Program& program, std::u16string_view input)
      : p_(program),
        code_(program.code.data()),
        in_(input),
        len_(static_cast<int32_t>(input.size())),
        unicode_(program.unicodeMode),
        caps_(2 * (program.captureCount + 1)),
        regs_(program.registerCount) {}

  // One attempt from `start`: the Pattern's closure applied to (Input, start).
  ExecStatus run(int32_t start, int32_t* end) {
    std::fill(caps_.begin(), caps_.end(), -1);
    stack_.clear();
    exhausted_ = false;
    uint32_t pc = 0;
    int32_t pos = start;
    for (;;) {
      const Inst& in = code_[pc];
      bool ok = true;
      switch (in.op) {
        case Op::Char:
        case Op::Set:
        case Op::Dot:
        case Op::Any:
          ok = matchOne(in, pos);
          pc++;
          break;
        case Op::LineStart:
          ok = pos == 0 || ((in.flags & kInstMultiline) && isLineTerminator(in_[pos - 1]));
          pc++;
          break;
        case Op::LineEnd:
          ok = pos == len_ || ((in.flags & kInstMultiline) && isLineTerminator(in_[pos]));
          pc++;
          break;
        case Op::WordBoundary:
        case Op::NotWordBoundary: {
          bool uw = in.flags & kUnicodeWords;
          bool a = pos > 0 && isWordUnit(in_[pos - 1], uw);
          bool b = pos < len_ && isWordUnit(in_[pos], uw);
          ok = (a != b) == (in.op == Op::WordBoundary);
          pc++;
          break;
        }
        case Op::Backref:
          ok = backreference(in, pos);
          pc++;
          break;
        case Op::Split:
          push({Entry::Choice, in.a, pos, 0});
          pc++;
          break;
        case Op::Jump:
          pc = in.a;
          break;
        case Op::GroupStart:
          setRegister(in.a, pos);
          pc++;
          break;
        case Op::GroupEnd: {
          int32_t s = regs_[in.b];
          if (in.flags & kBackward) {
            setCapture(in.a, pos, s);
          } else {
            setCapture(in.a, s, pos);
          }
          pc++;
          break;
        }
        case Op::ClearCaptures:
          for (uint32_t k = in.a; k < in.b; k++) {
            if (caps_[2 * k] != -1) setCapture(k, -1, -1);
          }
          pc++;
          break;
        case Op::LoopInit:
          setRegister(in.a, 0);
          pc++;
          break;
        case Op::Loop: {
          uint32_t count = static_cast<uint32_t>(regs_[in.a]);
          if (count < in.b) {
            pc++;  // below min: the atom only
          } else if (count >= in.c) {
            pc = in.d;
          } else if (in.flags & kGreedy) {
            push({Entry::Choice, in.d, pos, 0});
            pc++;
          } else {
            push({Entry::Choice, pc + 1, pos, 0});
            pc = in.d;
          }
          break;
        }
        case Op::LoopIter:
          setRegister(in.a + 1, pos);
          pc++;
          break;
        case Op::LoopNext: {
          uint32_t count = static_cast<uint32_t>(regs_[in.a]);
          // RepeatMatcher's empty check: past the minimum, an iteration that matched empty fails.
          if (count >= in.b && pos == regs_[in.a + 1]) {
            ok = false;
            break;
          }
          if (count != UINT32_MAX) setRegister(in.a, static_cast<int32_t>(count + 1));
          pc = in.c;
          break;
        }
        case Op::Star:
          ok = star(pc, pos);
          break;
        case Op::LookStart:
          if (!(in.flags & kNegative)) {
            // Backtracking past a positive lookaround restores its captures (the cut drops the body's undo).
            for (uint32_t k = in.c; k < in.d; k++) push({Entry::UndoCapture, k, caps_[2 * k], caps_[2 * k + 1]});
          }
          setRegister(in.a, 0);
          setRegister(in.a + 1, pos);
          regs_[in.a] = static_cast<int32_t>(stack_.size());  // where the barrier goes
          push({Entry::Barrier, pc, pos, 0});
          pc++;
          break;
        case Op::LookEnd: {
          const Inst& start = code_[in.b];
          size_t barrier = static_cast<size_t>(regs_[start.a]);
          if (!(start.flags & kNegative)) {
            stack_.resize(barrier);
            pos = regs_[start.a + 1];
            pc++;
          } else {
            unwindTo(barrier);
            ok = false;
          }
          break;
        }
        case Op::Match:
          *end = pos;
          return ExecStatus::Match;
      }
      if (exhausted_) return ExecStatus::ResourceExhausted;
      if (!ok && !backtrack(&pc, &pos)) return exhausted_ ? ExecStatus::ResourceExhausted : ExecStatus::NoMatch;
    }
  }

  const std::vector<int32_t>& captures() const { return caps_; }

 private:
  void push(Frame f) {
    if (stack_.size() >= kStackLimit) {
      exhausted_ = true;
      return;
    }
    stack_.push_back(f);
  }
  void setRegister(uint32_t r, int32_t value) {
    push({Entry::UndoRegister, r, regs_[r], 0});
    regs_[r] = value;
  }
  void setCapture(uint32_t k, int32_t s, int32_t e) {
    push({Entry::UndoCapture, k, caps_[2 * k], caps_[2 * k + 1]});
    caps_[2 * k] = s;
    caps_[2 * k + 1] = e;
  }

  // The character before or after pos, and its length in code units.
  bool readForward(int32_t pos, char32_t* c, int32_t* n) const {
    if (pos >= len_) return false;
    char32_t x = in_[pos];
    if (unicode_ && isLead(x) && pos + 1 < len_ && isTrail(in_[pos + 1])) {
      *c = 0x10000 + ((x - 0xD800) << 10) + (in_[pos + 1] - 0xDC00);
      *n = 2;
    } else {
      *c = x;
      *n = 1;
    }
    return true;
  }
  bool readBackward(int32_t pos, char32_t* c, int32_t* n) const {
    if (pos <= 0) return false;
    char32_t x = in_[pos - 1];
    if (unicode_ && isTrail(x) && pos >= 2 && isLead(in_[pos - 2])) {
      *c = 0x10000 + ((in_[pos - 2] - 0xD800) << 10) + (x - 0xDC00);
      *n = 2;
    } else {
      *c = x;
      *n = 1;
    }
    return true;
  }

  // CharacterSetMatcher (the set is already case-closed).
  bool matchOne(const Inst& in, int32_t& pos) const {
    char32_t c;
    int32_t n;
    bool backward = in.flags & kBackward;
    if (!(backward ? readBackward(pos, &c, &n) : readForward(pos, &c, &n))) return false;
    bool ok;
    switch (in.op) {
      case Op::Char:
        ok = c == in.a;
        break;
      case Op::Set:
        ok = p_.sets[in.a].contains(c);
        break;
      case Op::Dot:
        ok = !isLineTerminator(c);
        break;
      default:
        ok = true;
        break;
    }
    if (!ok) return false;
    pos += backward ? -n : n;
    return true;
  }

  // Back over one character a Star matched (the opposite of its direction).
  int32_t unstep(const Inst& matcher, int32_t pos) const {
    char32_t c = 0;
    int32_t n = 1;
    if (matcher.flags & kBackward) {
      readForward(pos, &c, &n);
      return pos + n;
    }
    readBackward(pos, &c, &n);
    return pos - n;
  }

  bool star(uint32_t& pc, int32_t& pos) {
    const Inst& in = code_[pc];
    const Inst& m = code_[pc + 1];
    int32_t p = pos;
    uint32_t n = 0;
    for (; n < in.b; n++) {
      if (!matchOne(m, p)) return false;
    }
    int32_t minPos = p;
    if (in.flags & kGreedy) {
      while (n < in.c && matchOne(m, p)) n++;
      if (p != minPos) push({Entry::StarGreedy, pc, p, minPos});
    } else if (in.c > in.b) {
      uint32_t more = in.c == UINT32_MAX ? UINT32_MAX : in.c - in.b;  // UINT32_MAX: unbounded
      push({Entry::StarLazy, pc, p, static_cast<int32_t>(more)});
    }
    pos = p;
    pc += 2;
    return true;
  }

  // BackreferenceMatcher: the first of the groups that participated (at most one did), compared character by
  // character, canonicalized with IgnoreCase.
  bool backreference(const Inst& in, int32_t& pos) const {
    int32_t rs = -1;
    int32_t re = -1;
    for (uint32_t i = in.a; i < in.a + in.b; i++) {
      uint32_t g = p_.backrefGroups[i];
      if (caps_[2 * g] != -1) {
        rs = caps_[2 * g];
        re = caps_[2 * g + 1];
        break;
      }
    }
    if (rs < 0) return true;
    bool backward = in.flags & kBackward;
    bool ignoreCase = in.flags & kInstIgnoreCase;
    CaseMode mode = unicode_ ? CaseMode::Unicode : CaseMode::Legacy;
    auto same = [&](char32_t a, char32_t b) { return a == b || (ignoreCase && canonicalize(mode, a) == canonicalize(mode, b)); };
    if (!unicode_) {
      int32_t n = re - rs;
      int32_t f = backward ? pos - n : pos + n;
      if (f < 0 || f > len_) return false;
      int32_t g = std::min(pos, f);
      for (int32_t i = 0; i < n; i++) {
        if (!same(in_[rs + i], in_[g + i])) return false;
      }
      pos = f;
      return true;
    }
    // Code points: the capture's count of them, from pos forward or backward.
    int32_t g = pos;
    if (backward) {
      char32_t c = 0;
      int32_t n = 1;
      for (int32_t q = rs; q < re; q += n) {
        readForward(q, &c, &n);
        int32_t m = 1;
        if (!readBackward(g, &c, &m)) return false;
        g -= m;
      }
    }
    int32_t q = rs;
    int32_t at = g;
    while (q < re) {
      char32_t a = 0;
      char32_t b = 0;
      int32_t n = 1;
      int32_t m = 1;
      readForward(q, &a, &n);
      if (!readForward(at, &b, &m) || !same(a, b)) return false;
      q += n;
      at += m;
    }
    pos = backward ? g : at;
    return true;
  }

  // Pops entries above `height`, applying the undo records only (a negative lookaround's body matched).
  void unwindTo(size_t height) {
    while (stack_.size() > height) {
      Frame f = stack_.back();
      stack_.pop_back();
      undo(f);
    }
  }
  void undo(const Frame& f) {
    if (f.kind == Entry::UndoCapture) {
      caps_[2 * f.pc] = f.a;
      caps_[2 * f.pc + 1] = f.b;
    } else if (f.kind == Entry::UndoRegister) {
      regs_[f.pc] = f.a;
    }
  }

  bool backtrack(uint32_t* pc, int32_t* pos) {
    while (!stack_.empty()) {
      Frame f = stack_.back();
      stack_.pop_back();
      switch (f.kind) {
        case Entry::Choice:
          *pc = f.pc;
          *pos = f.a;
          return true;
        case Entry::UndoCapture:
        case Entry::UndoRegister:
          undo(f);
          break;
        case Entry::Barrier: {
          const Inst& start = code_[f.pc];
          if (start.flags & kNegative) {
            // The body failed: the negative lookaround holds.
            *pc = start.b + 1;
            *pos = f.a;
            return true;
          }
          break;
        }
        case Entry::StarGreedy: {
          int32_t p = unstep(code_[f.pc + 1], f.a);
          if (p != f.b) push({Entry::StarGreedy, f.pc, p, f.b});
          *pc = f.pc + 2;
          *pos = p;
          return true;
        }
        case Entry::StarLazy: {
          int32_t p = f.a;
          if (!matchOne(code_[f.pc + 1], p)) break;
          uint32_t more = static_cast<uint32_t>(f.b);
          if (more != UINT32_MAX) more--;
          if (more > 0) push({Entry::StarLazy, f.pc, p, static_cast<int32_t>(more)});
          *pc = f.pc + 2;
          *pos = p;
          return true;
        }
      }
    }
    return false;
  }

  const Program& p_;
  const Inst* code_;
  std::u16string_view in_;
  int32_t len_;
  bool unicode_;
  std::vector<int32_t> caps_;
  std::vector<int32_t> regs_;
  std::vector<Frame> stack_;
  bool exhausted_ = false;
};

}  // namespace

ExecStatus exec(const Program& program, std::u16string_view input, size_t lastIndex, std::vector<Span>* spans) {
  if (input.size() > static_cast<size_t>(INT32_MAX) - 1) return ExecStatus::ResourceExhausted;
  size_t len = input.size();
  VM vm(program, input);
  for (size_t index = lastIndex;;) {
    if (index > len) return ExecStatus::NoMatch;
    // The character obtained from element lastIndex of S: with u/v, a trail surrogate's pair starts before it.
    size_t start = index;
    if (program.unicodeMode && index > 0 && index < len && isTrail(input[index]) && isLead(input[index - 1])) start--;
    if (program.anchoredStart && start != 0) return ExecStatus::NoMatch;
    int32_t end;
    ExecStatus status = vm.run(static_cast<int32_t>(start), &end);
    if (status == ExecStatus::Match) {
      const std::vector<int32_t>& caps = vm.captures();
      spans->assign(program.captureCount + 1, Span{});
      (*spans)[0] = {static_cast<int32_t>(index), end};
      for (uint32_t i = 1; i <= program.captureCount; i++) (*spans)[i] = {caps[2 * i], caps[2 * i + 1]};
      return status;
    }
    if (status == ExecStatus::ResourceExhausted || program.flags.sticky) return status;
    // AdvanceStringIndex.
    if (program.unicodeMode && index + 1 < len && isLead(input[index]) && isTrail(input[index + 1])) {
      index += 2;
    } else {
      index++;
    }
  }
}

}  // namespace snowman::regexp
