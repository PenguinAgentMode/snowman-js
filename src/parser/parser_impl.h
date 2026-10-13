// The recursive-descent parser's internals: shared by statements.cpp, expressions.cpp and scope.cpp.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "parser/ast.h"
#include "parser/lexer.h"
#include "parser/parser.h"

namespace snowman::parser {

using namespace ast;

// Positions (offsets) of cover-grammar constructs that are errors unless the cover turns out to be a
// pattern (or the other way round). -1: none.
struct DestructuringErrors {
  int64_t shorthandAssign = -1;      // `{a = 1}` in an expression
  int64_t trailingComma = -1;        // `[...a,]` as a pattern
  int64_t parenthesizedAssign = -1;  // `({a}) = 1`
  int64_t parenthesizedBind = -1;    // `((a)) => 1`
  int64_t doubleProto = -1;          // `{__proto__: 1, __proto__: 2}` in an expression
};

// What a lexical name in a scope was declared by.
enum class Lex : uint8_t { Let, Const, Using, Class, PlainFunction, OtherFunction, CatchSimple, CatchPattern, Import };

// How checkLVal* treats the identifiers it finds.
enum class Bind : uint8_t {
  None,     // an assignment target
  Var,      // var / for-in-of var
  Let,
  Const,
  Using,
  Class,
  Param,    // formal parameter: collected, checked when the body's strictness is known
  CatchSimple,
  CatchPattern,
  Import,
  Outside,  // validate only
};

struct Scope {
  enum class Kind : uint8_t { Root, Block, Catch, For, Switch };
  Kind kind = Kind::Block;
  Scope* parent = nullptr;
  bool moduleRoot = false;
  bool functionRoot = false;
  struct Entry {
    Lex kind;
    Node* node;
    uint32_t count;
  };
  std::unordered_map<std::u16string, Entry> lexical;
  std::unordered_set<std::u16string> vars;       // declared here or hoisted through here
  std::unordered_set<std::u16string> params;     // function roots
  std::unordered_set<std::u16string> functions;  // roots: top-level function declarations (var-like)
  std::vector<LexicalDeclaration>* annotation = nullptr;
  std::vector<FunctionDeclaration*> annexB;      // B.3.2 candidates still propagating outwards
};

// A function-like context: scripts, modules, functions, arrows, methods, field initializers, static blocks.
struct FnCtx {
  enum class Kind : uint8_t { Script, Module, Function, Arrow, Method, FieldInit, StaticBlock };
  FnCtx* parent = nullptr;
  Kind kind = Kind::Script;
  bool strict = false;
  bool async = false;
  bool generator = false;
  bool allowReturn = false;
  bool allowNewTarget = false;
  bool allowSuperProperty = false;
  bool allowSuperCall = false;
  bool allowArguments = true;
  bool inStaticBlock = false;  // `await` is reserved (crosses arrows, not functions)
  Scope* root = nullptr;
  VarScope* vs = nullptr;
  std::unordered_set<std::u16string> varNameSet;
  std::vector<FunctionDeclaration*> topFunctions;
  struct Label {
    std::u16string name;  // empty: an anonymous loop or switch
    uint8_t kind;         // 0 plain, 1 loop, 2 switch
    uint32_t statementStart;
  };
  std::vector<Label> labels;
  int loopDepth = 0;
  int switchDepth = 0;
  // 1-based source positions of the first yield / await expression and `await` identifier, for the
  // early errors of arrow parameters (cover grammar).
  uint32_t yieldPos = 0, awaitPos = 0, awaitIdentPos = 0;
  uint32_t evalCount = 0, argumentsCount = 0;
  bool usesTopLevelAwait = false;
};

struct PrivateScope {
  PrivateScope* parent = nullptr;
  // name -> bits: 1 getter, 2 setter, 4 other; 8 static, 16 instance
  std::unordered_map<std::u16string, uint8_t> declared;
  std::vector<std::pair<std::u16string, uint32_t>> used;
};

struct Pos {
  uint32_t start, line, col;
};

class Parser {
 public:
  Parser(std::u16string source, const ParseOptions& options);
  Program* parseProgram();
  // CreateDynamicFunction pieces: a FormalParameters / FunctionBody on its own.
  void checkDynamicParams(DynamicFunctionKind kind);
  void checkDynamicBody(DynamicFunctionKind kind);

  Program* program_ = nullptr;

 private:
  friend struct ScopeGuard;
  const ParseOptions& options_;
  const std::u16string& source_;
  Lexer lexer_;
  Token tok_;
  uint32_t prevEnd_ = 0;
  uint32_t prevStart_ = 0;
  bool module_;
  FnCtx* ctx_ = nullptr;
  Scope* scope_ = nullptr;
  PrivateScope* private_ = nullptr;
  int64_t potentialArrowAt_ = -1;
  bool potentialArrowInForAwait_ = false;
  std::vector<std::unique_ptr<FnCtx>> ctxPool_;
  std::vector<std::unique_ptr<Scope>> scopePool_;
  std::vector<std::unique_ptr<PrivateScope>> privatePool_;
  // Names collected by the current checkLVal* pass (BoundNames).
  std::vector<std::u16string>* collect_ = nullptr;
  std::vector<std::pair<std::u16string, uint32_t>>* collectParams_ = nullptr;
  // Modules: exported names (duplicates are errors) and `export {x}` bindings checked at the end.
  std::unordered_set<std::u16string> exportedNames_;
  std::vector<std::pair<std::u16string, uint32_t>> exportedBindings_;
  void addExportName(const std::u16string& name, uint32_t pos);
  std::u16string moduleExportNameString(Node* n);

  // --- tokens
  void next();
  Token peek();
  bool is(T t) const { return tok_.type == t; }
  bool isKw(K k) const { return tok_.type == T::Name && tok_.kw == k && !tok_.escaped; }
  bool isName() const { return tok_.type == T::Name; }
  bool eat(T t);
  bool eatKw(K k);
  void expect(T t);
  void expectKw(K k);
  bool canInsertSemicolon() const;
  void semicolon();
  [[noreturn]] void unexpected();
  [[noreturn]] void unexpectedAt(const Token& t);
  [[noreturn]] void raise(uint32_t pos, const std::string& message);
  // At each point the recursion passes through: a SyntaxError when the stack runs out (parser.h).
  void checkStack();
  Pos here() const { return {tok_.start, tok_.line, tok_.col}; }
  template <class N>
  N* startNode(Pos p) {
    N* n = make<N>();
    n->start = p.start;
    n->line = p.line;
    n->column = p.col;
    return n;
  }
  template <class N>
  N* startNode() {
    return startNode<N>(here());
  }
  template <class N>
  N* finish(N* n) {
    n->end = prevEnd_;
    return n;
  }
  template <class N>
  N* startAt(const Node* other) {
    N* n = make<N>();
    n->start = other->start;
    n->line = other->line;
    n->column = other->column;
    return n;
  }

  // --- contexts and scopes (scope.cpp)
  FnCtx* enterFn(FnCtx::Kind kind, bool async, bool generator, VarScope* vs);
  void exitFn();
  Scope* enterScope(Scope::Kind kind, std::vector<LexicalDeclaration>* annotation);
  void exitScope();
  void declareVarName(const std::u16string& name, uint32_t pos);
  void declareLexicalName(const std::u16string& name, Lex kind, Node* node, uint32_t pos);
  void declareFunction(FunctionDeclaration* fn, uint32_t pos);
  void addVarDeclarator(VariableDeclarator* d);
  void addLexicalDeclaration(Node* node, std::vector<std::u16string> names, bool isConstant);
  bool yieldIsKeyword() const { return ctx_->generator; }
  bool awaitIsKeyword() const;
  bool canAwait() const;
  bool inAsyncFunctionOrModule() const;
  void markDirectEval();
  void markArguments();
  void enterPrivateScope();
  void exitPrivateScope();
  void usePrivateName(const std::u16string& name, uint32_t pos);
  void finishFunctionAnnotations(Function* fn, FnCtx* ctx);
  void finishVarScope(VarScope* vs, FnCtx* ctx);

  // --- identifiers
  Identifier* parseIdent(bool liberal);
  Identifier* identFromToken();
  void checkUnreserved(const Token& t);
  void checkUnreservedName(const std::u16string& name, K kw, bool escaped, uint32_t pos, uint32_t end);
  PrivateIdentifier* parsePrivateIdent();

  // --- statements (statements.cpp)
  void parseTopLevel(Program* program);
  Node* parseStatement(const char* context, bool topLevel);
  void parseDirectives(std::vector<Statement*>& body, Function* fn, bool* becameStrict);
  BlockStatement* parseBlock(bool createScope, bool functionBody = false);
  Node* parseVarStatement(Pos start, VariableKind kind);
  VariableDeclaration* parseVar(Pos start, VariableKind kind, bool isFor, bool noIn);
  Node* parseFunctionStatement(Pos start, bool isAsync, bool declarationPosition, bool exportDefault);
  Function* parseFunction(Function* node, bool statement, bool nullableId, bool isAsync, bool hanging);
  void parseFunctionParams(Function* fn);
  void parseFunctionBody(Function* fn, bool isArrow, bool isMethod, bool noIn);
  void checkParams(Function* fn, bool allowDuplicates);
  Class* parseClass(Class* node, bool statement, bool nullableId);
  Node* parseClassElement(Class* cls, bool& hadConstructor);
  FunctionExpression* parseMethod(Pos start, bool isGenerator, bool isAsync, FunctionKind kind, bool allowSuperCall);
  Node* parseIf(Pos start);
  Node* parseFor(Pos start);
  Node* parseForInOf(Pos start, Node* init, bool isAwait, Scope* headScope);
  Node* parseWhile(Pos start);
  Node* parseDoWhile(Pos start);
  Node* parseSwitch(Pos start);
  Node* parseTry(Pos start);
  Node* parseReturn(Pos start);
  Node* parseThrow(Pos start);
  Node* parseWith(Pos start);
  Node* parseBreakContinue(Pos start, bool isBreak);
  Node* parseLabeled(Pos start, Identifier* label, const char* context);
  Node* parseExpressionStatement(Pos start, Node* expr);
  Node* parseImport(Pos start);
  Node* parseExport(Pos start);
  Node* parseModuleExportName();
  std::vector<ImportAttribute*> parseWithClause();
  Literal* parseModuleSpecifier();
  bool isLet(const char* context);
  bool isUsing(bool forHead);
  bool isAwaitUsing(bool forHead);
  bool isAsyncFunction();
  uint32_t addModuleRequest(Literal* source, const std::vector<ImportAttribute*>& attributes);
  void finishModule(Program* program);

  // --- expressions (expressions.cpp)
  Node* parseExpression(bool noIn = false, DestructuringErrors* refErrors = nullptr);
  Node* parseMaybeAssign(bool noIn = false, DestructuringErrors* refErrors = nullptr);
  Node* parseMaybeConditional(bool noIn, DestructuringErrors* refErrors);
  Node* parseExprOps(bool noIn, DestructuringErrors* refErrors);
  Node* parseExprOp(Node* left, Pos leftStart, int minPrec, bool noIn);
  Node* parseMaybeUnary(DestructuringErrors* refErrors, bool sawUnary, bool incDec, bool noIn);
  Node* parseExprSubscripts(DestructuringErrors* refErrors, bool noIn);
  Node* parseSubscripts(Node* base, Pos start, bool noCalls, bool noIn);
  Node* parseExprAtom(DestructuringErrors* refErrors, bool noIn, bool forNew = false);
  Node* parseParenAndDistinguish(bool canBeArrow, bool noIn);
  Node* parseArrowExpression(Pos start, std::vector<Node*> params, bool isAsync, bool noIn, uint32_t evalBefore,
                             uint32_t argsBefore);
  Node* parseNew();
  Node* parseImportExpr(bool forNew);
  Node* parseYield(bool noIn);
  Node* parseAwait(bool noIn);
  TemplateLiteral* parseTemplate(bool tagged);
  Node* parseObj(bool isPattern, DestructuringErrors* refErrors);
  Node* parseProperty(bool isPattern, DestructuringErrors* refErrors);
  Node* parsePropertyName(Property* prop);
  Node* parsePropertyName(bool& computed);
  std::vector<Node*> parseExprList(T close, bool allowTrailingComma, bool allowEmpty, DestructuringErrors* refErrors);
  Node* parseSpread(DestructuringErrors* refErrors);
  Literal* parseLiteralToken();
  Node* buildBinary(Pos start, Node* left, Node* right, T op);
  bool startsExpr() const;

  // --- patterns
  Node* parseBindingAtom();
  std::vector<Node*> parseBindingList(T close, bool allowEmpty, bool allowTrailingComma);
  Node* parseMaybeDefault(Pos start, Node* left);
  Node* parseRestBinding();
  Node* toAssignable(Node* node, bool isBinding, DestructuringErrors* refErrors);
  void toAssignableList(std::vector<Node*>& list, bool isBinding);
  void checkLValSimple(Node* expr, Bind bind);
  void checkLValPattern(Node* expr, Bind bind);
  void checkLValInnerPattern(Node* expr, Bind bind);
  void bindIdentifier(Identifier* id, Bind bind);
  bool checkPatternErrors(DestructuringErrors* refErrors, bool isAssign);
  bool checkExpressionErrors(DestructuringErrors* refErrors, bool andThrow);
  void checkYieldAwaitInDefaultParams();
  bool isSimpleAssignTarget(Node* n) const;
};

std::u16string_view sliceOf(const std::u16string& source, const Node* n);
void computeParamInfo(Function* fn);

}  // namespace snowman::parser
