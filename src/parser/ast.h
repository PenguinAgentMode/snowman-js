// The ESTree-shaped AST the parser produces and the interpreter walks. INTERFACES.md section 2.
//
// - One struct per ESTree node type (as acorn `ecmaVersion: "latest"` produces them), with ESTree field
//   names. Every node derives from Node; `node->is<T>()` / `node->as<T>()` test and downcast.
// - Nodes live in a process-wide arena and are never freed: they outlive every function created from them.
// - Strings are UTF-16 with escapes resolved (lone surrogates allowed). Positions are UTF-16 offsets.
// - Static semantics are computed by the parser and attached to the nodes below, named after the spec.
//   Fields marked "static semantics" are not part of ESTree and are not printed by --dump-ast.
// - The interpreter may cache data on any node through `interpCache`; the parser never touches it.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowman::ast {

#define SNOWMAN_AST_NODE_TYPES(X) \
  X(Program)                      \
  X(Identifier)                   \
  X(PrivateIdentifier)            \
  X(Literal)                      \
  X(ExpressionStatement)          \
  X(BlockStatement)               \
  X(EmptyStatement)               \
  X(DebuggerStatement)            \
  X(WithStatement)                \
  X(ReturnStatement)              \
  X(LabeledStatement)             \
  X(BreakStatement)               \
  X(ContinueStatement)            \
  X(IfStatement)                  \
  X(SwitchStatement)              \
  X(SwitchCase)                   \
  X(ThrowStatement)               \
  X(TryStatement)                 \
  X(CatchClause)                  \
  X(WhileStatement)               \
  X(DoWhileStatement)             \
  X(ForStatement)                 \
  X(ForInStatement)               \
  X(ForOfStatement)               \
  X(FunctionDeclaration)          \
  X(VariableDeclaration)          \
  X(VariableDeclarator)           \
  X(ClassDeclaration)             \
  X(ThisExpression)               \
  X(ArrayExpression)              \
  X(ObjectExpression)             \
  X(Property)                     \
  X(FunctionExpression)           \
  X(ArrowFunctionExpression)      \
  X(UnaryExpression)              \
  X(UpdateExpression)             \
  X(BinaryExpression)             \
  X(AssignmentExpression)         \
  X(LogicalExpression)            \
  X(MemberExpression)             \
  X(ConditionalExpression)        \
  X(CallExpression)               \
  X(NewExpression)                \
  X(SequenceExpression)           \
  X(YieldExpression)              \
  X(AwaitExpression)              \
  X(TemplateLiteral)              \
  X(TaggedTemplateExpression)     \
  X(TemplateElement)              \
  X(SpreadElement)                \
  X(Super)                        \
  X(MetaProperty)                 \
  X(ChainExpression)              \
  X(ImportExpression)             \
  X(ClassExpression)              \
  X(ObjectPattern)                \
  X(ArrayPattern)                 \
  X(RestElement)                  \
  X(AssignmentPattern)            \
  X(ClassBody)                    \
  X(MethodDefinition)             \
  X(PropertyDefinition)           \
  X(StaticBlock)                  \
  X(ImportDeclaration)            \
  X(ImportSpecifier)              \
  X(ImportDefaultSpecifier)       \
  X(ImportNamespaceSpecifier)     \
  X(ImportAttribute)              \
  X(ExportNamedDeclaration)       \
  X(ExportSpecifier)              \
  X(ExportDefaultDeclaration)     \
  X(ExportAllDeclaration)

enum class NodeType : uint8_t {
#define SNOWMAN_AST_ENUM(name) name,
  SNOWMAN_AST_NODE_TYPES(SNOWMAN_AST_ENUM)
#undef SNOWMAN_AST_ENUM
};

// "Program", "Identifier", ...: the ESTree type string.
const char* nodeTypeName(NodeType type);

struct Node {
  NodeType type;
  // Static semantics: the source wrapped this expression in parentheses (a ParenthesizedExpression in the
  // spec). ESTree drops the parentheses, but IsIdentifierRef and friends see them: `(f) = function(){}`
  // does not name the function.
  bool parenthesized = false;
  uint32_t start = 0, end = 0;    // [start, end) in UTF-16 code units of Program::source
  uint32_t line = 0, column = 0;  // of `start`: 1-based line, 0-based column in UTF-16 code units
  mutable void* interpCache = nullptr;

  template <class T>
  bool is() const {
    return T::classof(type);
  }
  template <class T>
  T* as() {
    assert(is<T>());
    return static_cast<T*>(this);
  }
  template <class T>
  const T* as() const {
    assert(is<T>());
    return static_cast<const T*>(this);
  }

 protected:
  explicit Node(NodeType t) : type(t) {}
};

#define SNOWMAN_AST_NODE(Name)                                          \
  static constexpr NodeType kType = NodeType::Name;                     \
  static bool classof(NodeType t) { return t == NodeType::Name; }       \
  Name() : Base(NodeType::Name) {}

// Documentation aliases: ESTree's interfaces are all plain nodes here.
using Expression = Node;
using Statement = Node;
using Pattern = Node;  // Identifier, MemberExpression (assignment targets), ObjectPattern, ArrayPattern,
                       // AssignmentPattern, RestElement

struct Identifier;
struct BlockStatement;
struct FunctionDeclaration;
struct VariableDeclarator;
struct SwitchCase;
struct CatchClause;
struct TemplateElement;
struct TemplateLiteral;
struct ClassBody;
struct MethodDefinition;
struct Literal;
struct Program;
struct ExportSpecifier;

// ---------------------------------------------------------------------------------------------------
// Static semantics shared by several node kinds.

// One element of LexicallyScopedDeclarations: a `let`/`const`/`using`/`await using` VariableDeclaration, a
// ClassDeclaration, a FunctionDeclaration (in a block, or at the top level of a module), or an
// ExportDefaultDeclaration of an expression (BoundNames «"*default*"»). `export default function(){}` and
// `export default class {}` appear as their FunctionDeclaration / ClassDeclaration (id null) with BoundNames
// «"*default*"».
struct LexicalDeclaration {
  Node* node = nullptr;
  std::vector<std::u16string> boundNames;
  bool isConstant = false;  // IsConstantDeclaration: const, using, await using
};

// Declarations of a var scope: a Program (script, module or eval code), a function, a class static block.
struct VarScope {
  bool strict = false;
  // VarDeclaredNames (deduplicated, in source order). For scripts and functions it includes the names of
  // the top-level function declarations (TopLevelVarDeclaredNames).
  std::vector<std::u16string> varNames;
  // VarScopedDeclarations: VariableDeclarator nodes (of `var`, including for-in/of heads) and, for scripts
  // and function bodies, the top-level FunctionDeclarations.
  std::vector<Node*> varScopedDeclarations;
  // LexicallyScopedDeclarations of the top level (TopLevelLexicallyScopedDeclarations for scripts and
  // functions; for modules this includes function declarations and imports are NOT included).
  std::vector<LexicalDeclaration> lexicallyScopedDeclarations;
  // functionsToInitialize (GlobalDeclarationInstantiation / FunctionDeclarationInstantiation /
  // EvalDeclarationInstantiation): the top-level function declarations, the last one per name, in source
  // order. Empty for modules (their functions are lexically scoped and listed above).
  std::vector<FunctionDeclaration*> functionsToInitialize;
  // Annex B.3.2: sloppy-mode function declarations directly in a Block, CaseClause or DefaultClause (or an
  // if-statement clause, B.3.3) inside this scope (not inside nested functions) for which replacing the
  // declaration with `var F` would produce no early error and (for functions) F is not a parameter name.
  // Each one also has FunctionDeclaration::annexBHoisted set. The runtime conditions of B.3.2.2 (global)
  // and B.3.2.3 (eval) are left to the evaluator.
  std::vector<FunctionDeclaration*> annexBFunctions;
  // A direct call `eval(...)` occurs in this scope's code (for functions: in the parameters or the body),
  // including inside nested arrow functions, not inside other nested functions, classes' methods or
  // static blocks.
  bool hasDirectEval = false;
};

// The LexicallyScopedDeclarations of a block-like scope.
struct BlockScope {
  std::vector<LexicalDeclaration> lexicallyScopedDeclarations;
};

// ---------------------------------------------------------------------------------------------------
// Identifiers and literals

struct Identifier : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(Identifier)
  std::u16string name;  // StringValue: escapes resolved
};

struct PrivateIdentifier : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(PrivateIdentifier)
  std::u16string name;  // without the leading '#'
};

enum class LiteralKind : uint8_t { Null, Boolean, Number, String, BigInt, RegExp };

struct RegExpValue {
  std::u16string pattern;  // the body as written (BodyText), no escapes resolved
  std::u16string flags;
};

struct Literal : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(Literal)
  LiteralKind kind = LiteralKind::Null;
  bool boolean = false;   // Boolean
  double number = 0;      // Number: the correctly rounded value
  std::u16string string;  // String: the SV (cooked value)
  std::string bigint;     // BigInt: decimal digits of the value (no sign, no separators, no `n`)
  RegExpValue regex;      // RegExp
  // ESTree's `raw` is Program::source.substr(start, end - start).
};

// ---------------------------------------------------------------------------------------------------
// Program

enum class SourceType : uint8_t { Script, Module };

struct ImportAttributeEntry {
  std::u16string key;
  std::u16string value;
};

// ModuleRequest Record.
struct ModuleRequest {
  std::u16string specifier;
  std::vector<ImportAttributeEntry> attributes;  // in source order; keys are unique
};

// ImportEntry Record. moduleRequest indexes Program::requestedModules.
struct ImportEntry {
  uint32_t moduleRequest = 0;
  bool isNamespaceObject = false;  // [[ImportName]] is NAMESPACE-OBJECT (`import * as ns`)
  std::u16string importName;       // when !isNamespaceObject
  std::u16string localName;
  Node* node = nullptr;            // the ImportSpecifier / ImportDefaultSpecifier / ImportNamespaceSpecifier
};

// ExportEntry Record.
struct ExportEntry {
  enum class ImportKind : uint8_t {
    None,           // [[ImportName]] null (a local export)
    Name,           // importName
    All,            // `export * as ns from` (ALL)
    AllButDefault,  // `export * from` (ALL-BUT-DEFAULT)
  };
  std::optional<std::u16string> exportName;
  std::optional<uint32_t> moduleRequest;  // index into Program::requestedModules
  ImportKind importKind = ImportKind::None;
  std::u16string importName;              // when importKind == Name
  std::optional<std::u16string> localName;
  Node* node = nullptr;                   // the declaration or specifier that produced it
};

struct Program : Node, VarScope {
  using Base = Node;
  SNOWMAN_AST_NODE(Program)
  std::vector<Statement*> body;  // statements and module declarations
  SourceType sourceType = SourceType::Script;
  // The whole source text, for Function.prototype.toString and Literal raw text. Never modified after
  // parsing, so string_views into it stay valid.
  std::u16string source;

  // Modules (static semantics).
  std::vector<ModuleRequest> requestedModules;  // ModuleRequests, deduplicated, in source order
  std::vector<ImportEntry> importEntries;
  std::vector<ExportEntry> localExportEntries;
  std::vector<ExportEntry> indirectExportEntries;
  std::vector<ExportEntry> starExportEntries;
  bool hasTopLevelAwait = false;  // [[HasTLA]]
};

// ---------------------------------------------------------------------------------------------------
// Statements

struct ExpressionStatement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ExpressionStatement)
  Expression* expression = nullptr;
  // A directive of a directive prologue: the raw text between the quotes (ESTree `directive`).
  std::optional<std::u16string> directive;
};

struct BlockStatement : Node, BlockScope {
  using Base = Node;
  SNOWMAN_AST_NODE(BlockStatement)
  std::vector<Statement*> body;
  // Static semantics: this block is the body of a function or static block. Its top-level declarations
  // are on the function (VarScope), not in lexicallyScopedDeclarations, which is empty.
  bool isFunctionBody = false;
};

struct EmptyStatement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(EmptyStatement)
};

struct DebuggerStatement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(DebuggerStatement)
};

struct WithStatement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(WithStatement)
  Expression* object = nullptr;
  Statement* body = nullptr;
};

struct ReturnStatement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ReturnStatement)
  Expression* argument = nullptr;  // nullable
};

struct LabeledStatement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(LabeledStatement)
  Identifier* label = nullptr;
  Statement* body = nullptr;
};

struct BreakStatement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(BreakStatement)
  Identifier* label = nullptr;  // nullable
};

struct ContinueStatement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ContinueStatement)
  Identifier* label = nullptr;  // nullable
};

// Annex B.3.3: in sloppy mode `if (x) function f(){}` is allowed; a FunctionDeclaration consequent or
// alternate behaves as if wrapped in a block of its own (evaluate it with a fresh block scope holding the
// function: see consequentScope / alternateScope).
struct IfStatement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(IfStatement)
  Expression* test = nullptr;
  Statement* consequent = nullptr;
  Statement* alternate = nullptr;  // nullable
  // Static semantics: the implicit block's declarations when consequent / alternate is a
  // FunctionDeclaration (B.3.3), else empty.
  BlockScope consequentScope;
  BlockScope alternateScope;
};

struct SwitchStatement : Node, BlockScope {  // lexicallyScopedDeclarations of the CaseBlock
  using Base = Node;
  SNOWMAN_AST_NODE(SwitchStatement)
  Expression* discriminant = nullptr;
  std::vector<SwitchCase*> cases;
};

struct SwitchCase : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(SwitchCase)
  Expression* test = nullptr;  // null for `default:`
  std::vector<Statement*> consequent;
};

struct ThrowStatement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ThrowStatement)
  Expression* argument = nullptr;
};

struct TryStatement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(TryStatement)
  BlockStatement* block = nullptr;
  CatchClause* handler = nullptr;      // nullable
  BlockStatement* finalizer = nullptr;  // nullable
};

struct CatchClause : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(CatchClause)
  Pattern* param = nullptr;  // nullable (optional catch binding)
  BlockStatement* body = nullptr;
  // Static semantics: BoundNames of the CatchParameter.
  std::vector<std::u16string> paramNames;
};

struct WhileStatement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(WhileStatement)
  Expression* test = nullptr;
  Statement* body = nullptr;
};

struct DoWhileStatement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(DoWhileStatement)
  Statement* body = nullptr;
  Expression* test = nullptr;
};

// The for-family's BlockScope holds the head's let/const/using declaration (at most one), else is empty.
struct ForStatement : Node, BlockScope {
  using Base = Node;
  SNOWMAN_AST_NODE(ForStatement)
  Node* init = nullptr;  // nullable: VariableDeclaration or Expression
  Expression* test = nullptr;    // nullable
  Expression* update = nullptr;  // nullable
  Statement* body = nullptr;
};

struct ForInStatement : Node, BlockScope {
  using Base = Node;
  SNOWMAN_AST_NODE(ForInStatement)
  Node* left = nullptr;  // VariableDeclaration or Pattern
  Expression* right = nullptr;
  Statement* body = nullptr;
};

struct ForOfStatement : Node, BlockScope {
  using Base = Node;
  SNOWMAN_AST_NODE(ForOfStatement)
  Node* left = nullptr;  // VariableDeclaration or Pattern
  Expression* right = nullptr;
  Statement* body = nullptr;
  bool await = false;  // for await (... of ...)
};

// ---------------------------------------------------------------------------------------------------
// Functions

enum class FunctionKind : uint8_t {
  Normal,              // function declarations and expressions (incl. generators / async)
  Arrow,               // [[ThisMode]] lexical
  Method,              // object literal or class method (incl. generator / async methods)
  Getter,
  Setter,
  ClassConstructor,    // explicit constructor of a base class ([[ConstructorKind]] base)
  DerivedConstructor,  // explicit constructor of a class with `extends` ([[ConstructorKind]] derived)
};

// FunctionDeclaration, FunctionExpression, ArrowFunctionExpression. Method bodies are the FunctionExpression
// value of a Property / MethodDefinition. `node->is<Function>()` matches all three.
struct Function : Node, VarScope {
  Identifier* id = nullptr;  // nullable
  std::vector<Pattern*> params;
  Node* body = nullptr;  // BlockStatement, or an Expression for a concise arrow body (expression == true)
  bool generator = false;
  bool async = false;
  bool expression = false;  // arrow with an expression body

  // Static semantics (VarScope above holds strict, varNames, declarations, functionsToInitialize, ...).
  FunctionKind kind = FunctionKind::Normal;
  bool hasSimpleParameterList = true;
  bool hasParameterExpressions = false;  // ContainsExpression of the formals
  bool hasDuplicateParameters = false;   // parameterNames has duplicates (sloppy simple lists only)
  // FunctionDeclarationInstantiation steps 15-18: false for arrows, when a parameter is named
  // "arguments", or (no parameter expressions and) a top-level function or lexical name is "arguments".
  bool argumentsObjectNeeded = true;
  // `arguments` is referenced as an identifier in the parameters or body (incl. nested arrows), or
  // hasDirectEval. When false the arguments object is unobservable and may be skipped.
  bool referencesArguments = false;
  uint32_t expectedArgumentCount = 0;  // ExpectedArgumentCount: the `length` property
  std::vector<std::u16string> parameterNames;  // BoundNames of the formals, in order, with duplicates
  // Annex B.3.2: a block-level FunctionDeclaration that is a var-hoisting candidate (see VarScope).
  bool annexBHoisted = false;
  // The source text matched by the function (for methods: the whole MethodDefinition including `get`,
  // `async`, `*`, but not `static`), a view into the owning Program::source.
  std::u16string_view sourceText;

  bool isAsync() const { return async; }
  bool isGenerator() const { return generator; }
  bool isArrow() const { return kind == FunctionKind::Arrow; }
  bool isClassConstructor() const {
    return kind == FunctionKind::ClassConstructor || kind == FunctionKind::DerivedConstructor;
  }

  static bool classof(NodeType t) {
    return t == NodeType::FunctionDeclaration || t == NodeType::FunctionExpression ||
           t == NodeType::ArrowFunctionExpression;
  }

 protected:
  explicit Function(NodeType t) : Node(t) {}
};

struct FunctionDeclaration : Function {
  using Base = Function;
  SNOWMAN_AST_NODE(FunctionDeclaration)
};

struct FunctionExpression : Function {
  using Base = Function;
  SNOWMAN_AST_NODE(FunctionExpression)
};

struct ArrowFunctionExpression : Function {
  using Base = Function;
  SNOWMAN_AST_NODE(ArrowFunctionExpression)
};

// ---------------------------------------------------------------------------------------------------
// Declarations

enum class VariableKind : uint8_t { Var, Let, Const, Using, AwaitUsing };

struct VariableDeclaration : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(VariableDeclaration)
  std::vector<VariableDeclarator*> declarations;
  VariableKind kind = VariableKind::Var;
};

struct VariableDeclarator : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(VariableDeclarator)
  Pattern* id = nullptr;
  Expression* init = nullptr;  // nullable
};

// ---------------------------------------------------------------------------------------------------
// Classes

// ClassDeclaration and ClassExpression; `node->is<Class>()` matches both.
struct Class : Node {
  Identifier* id = nullptr;            // nullable
  Expression* superClass = nullptr;    // nullable
  ClassBody* body = nullptr;

  // Static semantics.
  MethodDefinition* constructor = nullptr;  // the explicit `constructor(...) {}`, or null
  // PrivateBoundIdentifiers of the body, deduplicated (a getter/setter pair is one name), without '#'.
  std::vector<std::u16string> privateBoundIdentifiers;
  std::u16string_view sourceText;  // the whole class, for the constructor's Function.prototype.toString

  static bool classof(NodeType t) { return t == NodeType::ClassDeclaration || t == NodeType::ClassExpression; }

 protected:
  explicit Class(NodeType t) : Node(t) {}
};

struct ClassDeclaration : Class {
  using Base = Class;
  SNOWMAN_AST_NODE(ClassDeclaration)
};

struct ClassExpression : Class {
  using Base = Class;
  SNOWMAN_AST_NODE(ClassExpression)
};

struct ClassBody : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ClassBody)
  std::vector<Node*> body;  // MethodDefinition, PropertyDefinition, StaticBlock
};

enum class MethodKind : uint8_t { Constructor, Method, Get, Set };

struct MethodDefinition : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(MethodDefinition)
  Expression* key = nullptr;  // Identifier, PrivateIdentifier, Literal, or an expression if computed
  FunctionExpression* value = nullptr;
  MethodKind kind = MethodKind::Method;
  bool computed = false;
  bool isStatic = false;  // ESTree `static`
};

// A class field. Its initializer runs as a method of its own (ClassFieldDefinition): `this` is the
// instance, `arguments` is an early error, `super.x` is allowed.
struct PropertyDefinition : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(PropertyDefinition)
  Expression* key = nullptr;
  Expression* value = nullptr;  // nullable
  bool computed = false;
  bool isStatic = false;  // ESTree `static`
  // Static semantics: the initializer contains a direct eval (incl. nested arrows).
  bool hasDirectEval = false;
};

// `static { ... }`: runs as a method with a var scope of its own (ClassStaticBlockDefinition).
struct StaticBlock : Node, VarScope {
  using Base = Node;
  SNOWMAN_AST_NODE(StaticBlock)
  std::vector<Statement*> body;
};

// ---------------------------------------------------------------------------------------------------
// Expressions

struct ThisExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ThisExpression)
};

struct Super : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(Super)
};

struct ArrayExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ArrayExpression)
  std::vector<Expression*> elements;  // null for holes; SpreadElement for `...x`
};

enum class PropertyKind : uint8_t { Init, Get, Set };

// An object literal member, or (in an ObjectPattern) a property pattern.
struct Property : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(Property)
  Expression* key = nullptr;  // Identifier, Literal, or an expression if computed
  Node* value = nullptr;      // Expression (FunctionExpression for methods/accessors) or Pattern
  PropertyKind kind = PropertyKind::Init;
  bool method = false;
  bool shorthand = false;
  bool computed = false;
};

struct ObjectExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ObjectExpression)
  std::vector<Node*> properties;  // Property, SpreadElement
};

enum class UnaryOperator : uint8_t { Minus, Plus, Not, BitNot, Typeof, Void, Delete };
enum class UpdateOperator : uint8_t { Increment, Decrement };
enum class BinaryOperator : uint8_t {
  Equal, NotEqual, StrictEqual, StrictNotEqual, Less, LessEqual, Greater, GreaterEqual,
  ShiftLeft, ShiftRight, ShiftRightUnsigned, Add, Subtract, Multiply, Divide, Remainder, Exponent,
  BitOr, BitXor, BitAnd, In, Instanceof,
};
enum class LogicalOperator : uint8_t { Or, And, Coalesce };
enum class AssignmentOperator : uint8_t {
  Assign, Add, Subtract, Multiply, Divide, Remainder, Exponent, ShiftLeft, ShiftRight, ShiftRightUnsigned,
  BitOr, BitXor, BitAnd, Or, And, Coalesce,  // ||= &&= ??=
};

// The operator's source text ("-", "typeof", "+=", ...).
const char* operatorText(UnaryOperator op);
const char* operatorText(UpdateOperator op);
const char* operatorText(BinaryOperator op);
const char* operatorText(LogicalOperator op);
const char* operatorText(AssignmentOperator op);

struct UnaryExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(UnaryExpression)
  UnaryOperator op = UnaryOperator::Minus;  // ESTree `operator`
  bool prefix = true;
  Expression* argument = nullptr;
};

struct UpdateExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(UpdateExpression)
  UpdateOperator op = UpdateOperator::Increment;  // ESTree `operator`
  bool prefix = false;
  Expression* argument = nullptr;  // Identifier or MemberExpression
};

struct BinaryExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(BinaryExpression)
  BinaryOperator op = BinaryOperator::Add;  // ESTree `operator`
  Expression* left = nullptr;  // a PrivateIdentifier for `#x in obj`
  Expression* right = nullptr;
};

struct LogicalExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(LogicalExpression)
  LogicalOperator op = LogicalOperator::Or;  // ESTree `operator`
  Expression* left = nullptr;
  Expression* right = nullptr;
};

struct AssignmentExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(AssignmentExpression)
  AssignmentOperator op = AssignmentOperator::Assign;  // ESTree `operator`
  Pattern* left = nullptr;  // for `=`: any Pattern; otherwise Identifier or MemberExpression
  Expression* right = nullptr;
};

struct MemberExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(MemberExpression)
  Expression* object = nullptr;    // may be Super
  Expression* property = nullptr;  // Identifier (non-computed), PrivateIdentifier, or an expression
  bool computed = false;
  bool optional = false;  // `a?.b`: this link starts an optional chain (inside a ChainExpression)
};

struct ConditionalExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ConditionalExpression)
  Expression* test = nullptr;
  Expression* consequent = nullptr;
  Expression* alternate = nullptr;
};

struct CallExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(CallExpression)
  Expression* callee = nullptr;  // Super for `super(...)`
  std::vector<Expression*> arguments;  // may contain SpreadElement
  bool optional = false;  // `f?.()`
  // Static semantics: a direct eval candidate (callee is the Identifier `eval`, possibly parenthesized,
  // not optional). Whether it is direct is decided at run time (SameValue with %eval%).
  bool isDirectEvalCandidate = false;
};

struct NewExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(NewExpression)
  Expression* callee = nullptr;
  std::vector<Expression*> arguments;
};

struct SequenceExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(SequenceExpression)
  std::vector<Expression*> expressions;
};

struct YieldExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(YieldExpression)
  Expression* argument = nullptr;  // nullable
  bool delegate = false;           // yield*
};

struct AwaitExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(AwaitExpression)
  Expression* argument = nullptr;
};

struct TemplateElement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(TemplateElement)
  std::u16string raw;                   // TRV: as written, CR and CRLF normalized to LF
  std::optional<std::u16string> cooked; // TV; absent for an invalid escape in a tagged template
  bool tail = false;
};

struct TemplateLiteral : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(TemplateLiteral)
  std::vector<TemplateElement*> quasis;
  std::vector<Expression*> expressions;  // quasis.size() == expressions.size() + 1
};

// The interpreter's per-site template object cache (GetTemplateObject) can use interpCache.
struct TaggedTemplateExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(TaggedTemplateExpression)
  Expression* tag = nullptr;
  TemplateLiteral* quasi = nullptr;
};

struct SpreadElement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(SpreadElement)
  Expression* argument = nullptr;
};

// new.target, import.meta
struct MetaProperty : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(MetaProperty)
  Identifier* meta = nullptr;
  Identifier* property = nullptr;
};

// The outermost node of an optional chain `a?.b.c()`: short-circuits to undefined.
struct ChainExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ChainExpression)
  Expression* expression = nullptr;  // MemberExpression or CallExpression
};

// import(source [, options])
struct ImportExpression : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ImportExpression)
  Expression* source = nullptr;
  Expression* options = nullptr;  // nullable
};

// ---------------------------------------------------------------------------------------------------
// Patterns

struct ObjectPattern : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ObjectPattern)
  std::vector<Node*> properties;  // Property (value is a Pattern), RestElement last
};

struct ArrayPattern : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ArrayPattern)
  std::vector<Pattern*> elements;  // null for elisions; RestElement last
};

struct RestElement : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(RestElement)
  Pattern* argument = nullptr;
};

struct AssignmentPattern : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(AssignmentPattern)
  Pattern* left = nullptr;
  Expression* right = nullptr;
};

// ---------------------------------------------------------------------------------------------------
// Modules

struct ImportAttribute : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ImportAttribute)
  Node* key = nullptr;      // Identifier or string Literal
  Literal* value = nullptr;  // string Literal
};

struct ImportDeclaration : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ImportDeclaration)
  std::vector<Node*> specifiers;  // ImportSpecifier, ImportDefaultSpecifier, ImportNamespaceSpecifier
  Literal* source = nullptr;
  std::vector<ImportAttribute*> attributes;
};

struct ImportSpecifier : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ImportSpecifier)
  Node* imported = nullptr;  // Identifier or string Literal
  Identifier* local = nullptr;
};

struct ImportDefaultSpecifier : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ImportDefaultSpecifier)
  Identifier* local = nullptr;
};

struct ImportNamespaceSpecifier : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ImportNamespaceSpecifier)
  Identifier* local = nullptr;
};

struct ExportNamedDeclaration : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ExportNamedDeclaration)
  Node* declaration = nullptr;  // nullable: VariableDeclaration, FunctionDeclaration, ClassDeclaration
  std::vector<ExportSpecifier*> specifiers;
  Literal* source = nullptr;  // nullable
  std::vector<ImportAttribute*> attributes;
};

struct ExportSpecifier : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ExportSpecifier)
  Node* local = nullptr;     // Identifier, or string Literal (only with `from`)
  Node* exported = nullptr;  // Identifier or string Literal
};

struct ExportDefaultDeclaration : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ExportDefaultDeclaration)
  Node* declaration = nullptr;  // FunctionDeclaration / ClassDeclaration (id may be null), or Expression
};

struct ExportAllDeclaration : Node {
  using Base = Node;
  SNOWMAN_AST_NODE(ExportAllDeclaration)
  Node* exported = nullptr;  // nullable: Identifier or string Literal for `export * as x from`
  Literal* source = nullptr;
  std::vector<ImportAttribute*> attributes;
};

#undef SNOWMAN_AST_NODE

// ---------------------------------------------------------------------------------------------------
// Allocation: a process-wide bump arena. Nodes are never destroyed.

void* arenaAllocate(size_t size, size_t align);

template <class T, class... Args>
T* make(Args&&... args) {
  return new (arenaAllocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
}

}  // namespace snowman::ast
