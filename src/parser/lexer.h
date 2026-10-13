// The tokenizer: UTF-16 source in, tokens out. Driven by the parser, which decides where a `/` starts a
// RegExp literal and where a `}` continues a template (rescanRegExp / rescanTemplate).
#pragma once

#include <cstdint>
#include <string>

#include "parser/parser.h"

namespace snowman::parser {

enum class T : uint8_t {
  EOS, Name, PrivateName, Num, BigInt, Str, Template, RegExp,
  LBrace, RBrace, LParen, RParen, LBracket, RBracket, Dot, Ellipsis, Semi, Comma, Colon, Question,
  QuestionDot, Arrow,
  Lt, Gt, LtEq, GtEq, Eq2, NotEq, Eq3, NotEq2,
  Plus, Minus, Star, Slash, Percent, Star2, Inc, Dec, Shl, Sar, Shr, Amp, Pipe, Caret, Bang, Tilde,
  And, Or, Nullish,
  Assign, PlusEq, MinusEq, StarEq, SlashEq, PercentEq, Star2Eq, ShlEq, SarEq, ShrEq, AmpEq, PipeEq, CaretEq,
  AndEq, OrEq, NullishEq,
  At,  // `@` (decorators)
};

// Keyword ids of Name tokens (by StringValue, escaped or not).
enum class K : uint8_t {
  None,
  // ReservedWord
  Await, Break, Case, Catch, Class, Const, Continue, Debugger, Default, Delete, Do, Else, Enum, Export, Extends,
  False, Finally, For, Function, If, Import, In, Instanceof, New, Null, Return, Super, Switch, This, Throw, True,
  Try, Typeof, Var, Void, While, With, Yield,
  // reserved in strict mode code
  Implements, Interface, Let, Package, Private, Protected, Public, Static,
  // contextual
  As, Async, From, Get, Of, Set, Target, Meta, Using, Eval, Arguments, Constructor, Prototype, Proto,
};

// ReservedWord other than await and yield: never an Identifier.
inline bool isAlwaysReserved(K k) { return k >= K::Break && k <= K::With; }
inline bool isStrictReserved(K k) { return (k >= K::Implements && k <= K::Static) || k == K::Yield; }

struct SyntaxErrorException {
  ParseError error;
};

struct Token {
  T type = T::EOS;
  K kw = K::None;
  bool escaped = false;      // Name / PrivateName: contains a \u escape
  bool nlBefore = false;     // a LineTerminator precedes this token
  bool legacyOctal = false;  // Num: LegacyOctalIntegerLiteral / NonOctalDecimalIntegerLiteral (sloppy only)
  bool templateTail = false; // Template: ends with ` rather than ${
  bool cookedInvalid = false;  // Template: an invalid escape (cooked is undefined)
  int32_t octalEscape = -1;  // Str / Template: offset of the first legacy octal or \8 \9 escape
  uint32_t start = 0, end = 0, line = 1, col = 0;
  uint32_t invalidEscapePos = 0;
  double num = 0;
  std::u16string str;  // Name: StringValue; Str: SV; Template: TV; RegExp: body; PrivateName: name
  std::u16string raw;  // Template: TRV; RegExp: flags
  std::string bigint;  // BigInt: decimal digits
};

class Lexer {
 public:
  Lexer(const char16_t* src, uint32_t length, bool module, bool hashbang);

  struct State {
    uint32_t pos, line, lineStart;
  };
  State save() const { return {pos_, line_, lineStart_}; }
  void restore(State s) {
    pos_ = s.pos;
    line_ = s.line;
    lineStart_ = s.lineStart;
  }

  // The next token after the current position (`/` lexes as division).
  void next(Token& tok);
  // `tok` is a `/` or `/=` token: lex it again as a RegularExpressionLiteral.
  void rescanRegExp(Token& tok);
  // `tok` is a `}` token closing a template substitution: lex it again as a TemplateMiddle / TemplateTail.
  void rescanTemplate(Token& tok);

  [[noreturn]] void fail(uint32_t pos, const std::string& message) const;
  void lineColumnOf(uint32_t pos, uint32_t& line, uint32_t& column) const;

 private:
  const char16_t* src_;
  uint32_t len_;
  uint32_t pos_ = 0;
  uint32_t line_ = 1, lineStart_ = 0;
  bool module_;

  char32_t peekCodePoint(uint32_t at, uint32_t& size) const;
  void skipTrivia(Token& tok);
  void newline(uint32_t at);  // a line terminator at `at` (pos_ already past it)
  void lexIdentifier(Token& tok, bool privateName);
  char32_t lexUnicodeEscape(uint32_t& p, bool& ok);
  void lexNumber(Token& tok);
  void lexString(Token& tok, char16_t quote);
  void lexTemplate(Token& tok, uint32_t from);
  void lexPunctuator(Token& tok);
};

bool isLineTerminator(char32_t c);
bool isWhiteSpace(char32_t c);
bool isIdentifierStartCP(char32_t c);
bool isIdentifierPartCP(char32_t c);
K keywordOf(std::u16string_view name);

}  // namespace snowman::parser
