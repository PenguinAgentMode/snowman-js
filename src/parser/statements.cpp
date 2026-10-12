// Statements, declarations, functions and classes.
#include <algorithm>
#include <cstring>

#include "parser/parser_impl.h"

namespace snowman::parser {

namespace {

std::string utf8(std::u16string_view s) { return utf16ToUtf8(s); }

bool hasContext(const char* context, const char* word) { return context && std::strstr(context, word); }

}  // namespace

void Parser::parseTopLevel(Program* program) {
  parseDirectives(program->body, nullptr, nullptr);
  while (!is(T::EOS)) program->body.push_back(parseStatement(nullptr, true));
}

// A directive prologue: string-literal expression statements at the start of a body. Sets strict mode on
// "use strict" (fn: the function whose body this is, for the non-simple-parameters error).
void Parser::parseDirectives(std::vector<Statement*>& body, Function* fn, bool* becameStrict) {
  int64_t octalPos = -1;
  while (is(T::Str)) {
    Token strTok = tok_;
    Node* stmt = parseStatement(nullptr, false);
    body.push_back(stmt);
    auto* es = stmt->is<ExpressionStatement>() ? stmt->as<ExpressionStatement>() : nullptr;
    Node* expr = es ? es->expression : nullptr;
    if (!expr || !expr->is<Literal>() || expr->parenthesized || expr->start != strTok.start || expr->end != strTok.end)
      break;
    es->directive = std::u16string(source_.substr(strTok.start + 1, strTok.end - strTok.start - 2));
    if (strTok.octalEscape >= 0 && octalPos < 0) octalPos = strTok.octalEscape;
    if (*es->directive == u"use strict") {
      if (fn && !fn->hasSimpleParameterList)
        raise(strTok.start, "Illegal 'use strict' directive in function with non-simple parameter list");
      if (!ctx_->strict) {
        ctx_->strict = true;
        if (becameStrict) *becameStrict = true;
      }
      if (octalPos >= 0) raise(static_cast<uint32_t>(octalPos), "Octal escape sequences are not allowed in strict mode");
    }
  }
}

bool Parser::isLet(const char* context) {
  if (!isKw(K::Let)) return false;
  Token nt = peek();
  if (nt.type == T::LBracket) return true;
  if (context) return false;
  if (nt.type == T::LBrace) return true;
  if (nt.type == T::Name) {
    if (!nt.escaped && (nt.kw == K::In || nt.kw == K::Instanceof)) return false;
    return true;
  }
  return false;
}

bool Parser::isAsyncFunction() {
  if (!isKw(K::Async)) return false;
  Token nt = peek();
  return nt.type == T::Name && nt.kw == K::Function && !nt.escaped && !nt.nlBefore;
}

// `using x` (not `using in`, `using of` in for heads where it would be a for-of over `using`).
bool Parser::isUsing(bool forHead) {
  if (!isKw(K::Using)) return false;
  Token nt = peek();
  if (nt.type != T::Name || nt.nlBefore) return false;
  if (!nt.escaped && (nt.kw == K::In || nt.kw == K::Instanceof)) return false;
  if (forHead && !nt.escaped && nt.kw == K::Of) {
    // `for (using of ...)`: a for-of over the identifier `using`, unless `for (using of of ...)`.
    Lexer::State state = lexer_.save();
    Token t1, t2;
    lexer_.next(t1);
    lexer_.next(t2);
    lexer_.restore(state);
    return t2.type == T::Name && !t2.escaped && t2.kw == K::Of && false;
  }
  return true;
}

bool Parser::isAwaitUsing(bool forHead) {
  (void)forHead;
  if (!isKw(K::Await) || !canAwait()) return false;
  Lexer::State state = lexer_.save();
  Token t1, t2;
  lexer_.next(t1);
  lexer_.next(t2);
  lexer_.restore(state);
  if (!(t1.type == T::Name && t1.kw == K::Using && !t1.escaped && !t1.nlBefore)) return false;
  if (t2.type != T::Name || t2.nlBefore) return false;
  if (!t2.escaped && (t2.kw == K::In || t2.kw == K::Instanceof)) return false;
  return true;
}

Node* Parser::parseStatement(const char* context, bool topLevel) {
  Pos start = here();
  if (isLet(context)) {
    if (context) unexpected();
    return parseVarStatement(start, VariableKind::Let);
  }
  if (isName() && !tok_.escaped) {
    switch (tok_.kw) {
      case K::Break:
      case K::Continue:
        return parseBreakContinue(start, tok_.kw == K::Break);
      case K::Debugger: {
        auto* node = startNode<DebuggerStatement>(start);
        next();
        semicolon();
        return finish(node);
      }
      case K::Do:
        return parseDoWhile(start);
      case K::For:
        return parseFor(start);
      case K::Function:
        if (context && (ctx_->strict || (std::strcmp(context, "if") != 0 && std::strcmp(context, "label") != 0)))
          unexpected();
        return parseFunctionStatement(start, false, !context, false);
      case K::Class:
        if (context) unexpected();
        return parseClass(startNode<ClassDeclaration>(start), true, false);
      case K::If:
        return parseIf(start);
      case K::Return:
        return parseReturn(start);
      case K::Switch:
        return parseSwitch(start);
      case K::Throw:
        return parseThrow(start);
      case K::Try:
        return parseTry(start);
      case K::Const:
        if (context) unexpected();
        return parseVarStatement(start, VariableKind::Const);
      case K::Var:
        return parseVarStatement(start, VariableKind::Var);
      case K::While:
        return parseWhile(start);
      case K::With:
        return parseWith(start);
      case K::Import: {
        Token nt = peek();
        if (nt.type == T::LParen || nt.type == T::Dot) break;
        if (!module_) raise(tok_.start, "Cannot use import statement outside a module");
        if (!topLevel) raise(tok_.start, "'import' and 'export' may only appear at the top level");
        return parseImport(start);
      }
      case K::Export:
        if (!module_) raise(tok_.start, "Unexpected token 'export'");
        if (!topLevel) raise(tok_.start, "'import' and 'export' may only appear at the top level");
        return parseExport(start);
      default:
        break;
    }
  }
  if (is(T::LBrace)) return parseBlock(true);
  if (is(T::Semi)) {
    auto* node = startNode<EmptyStatement>(start);
    next();
    return finish(node);
  }
  if (isAsyncFunction()) {
    if (context) unexpected();
    next();
    return parseFunctionStatement(start, true, true, false);
  }
  bool usingDecl = isUsing(false);
  bool awaitUsing = !usingDecl && isAwaitUsing(false);
  if (usingDecl || awaitUsing) {
    if (context) unexpected();
    if (!module_ && ctx_->kind == FnCtx::Kind::Script && scope_->kind == Scope::Kind::Root)
      raise(tok_.start, "Using declaration cannot appear in the top level of a script");
    if (scope_->kind == Scope::Kind::Switch) raise(tok_.start, "Using declaration cannot appear in a case clause");
    if (awaitUsing) {
      next();
      if (ctx_->kind == FnCtx::Kind::Module && scope_ == ctx_->root) ctx_->usesTopLevelAwait = true;
      if (ctx_->kind == FnCtx::Kind::Module) ctx_->usesTopLevelAwait = true;
    }
    next();
    VariableDeclaration* decl = parseVar(start, awaitUsing ? VariableKind::AwaitUsing : VariableKind::Using, false, false);
    semicolon();
    return finish(decl);
  }
  Token first = tok_;
  Node* expr = parseExpression();
  if (first.type == T::Name && expr->is<Identifier>() && !expr->parenthesized && expr->start == first.start &&
      expr->end == first.end && is(T::Colon)) {
    next();
    return parseLabeled(start, expr->as<Identifier>(), context);
  }
  return parseExpressionStatement(start, expr);
}

Node* Parser::parseExpressionStatement(Pos start, Node* expr) {
  auto* node = startNode<ExpressionStatement>(start);
  node->expression = expr;
  semicolon();
  return finish(node);
}

BlockStatement* Parser::parseBlock(bool createScope, bool functionBody) {
  auto* node = startNode<BlockStatement>();
  node->isFunctionBody = functionBody;
  expect(T::LBrace);
  if (createScope) {
    enterScope(Scope::Kind::Block, &node->lexicallyScopedDeclarations);
  } else if (!functionBody) {
    scope_->annotation = &node->lexicallyScopedDeclarations;
  }
  while (!is(T::RBrace)) {
    if (is(T::EOS)) unexpected();
    node->body.push_back(parseStatement(nullptr, false));
  }
  next();
  if (createScope) exitScope();
  return finish(node);
}

Node* Parser::parseVarStatement(Pos start, VariableKind kind) {
  next();
  VariableDeclaration* decl = parseVar(start, kind, false, false);
  semicolon();
  return finish(decl);
}

VariableDeclaration* Parser::parseVar(Pos start, VariableKind kind, bool isFor, bool noIn) {
  auto* node = startNode<VariableDeclaration>(start);
  node->kind = kind;
  std::vector<std::u16string> names;
  Bind bind = kind == VariableKind::Var     ? Bind::Var
              : kind == VariableKind::Let   ? Bind::Let
              : kind == VariableKind::Const ? Bind::Const
                                            : Bind::Using;
  for (;;) {
    auto* d = startNode<VariableDeclarator>();
    bool isUsingKind = kind == VariableKind::Using || kind == VariableKind::AwaitUsing;
    if (isUsingKind && (is(T::LBracket) || is(T::LBrace))) unexpected();
    d->id = parseBindingAtom();
    std::vector<std::u16string>* saved = collect_;
    collect_ = &names;
    checkLValPattern(d->id, bind);
    collect_ = saved;
    bool forInOf = isFor && (isKw(K::In) || isKw(K::Of));
    if (eat(T::Assign)) {
      d->init = parseMaybeAssign(noIn);
    } else if (kind == VariableKind::Const && !forInOf) {
      raise(tok_.start, "Missing initializer in const declaration");
    } else if (isUsingKind && !(isFor && isKw(K::Of))) {
      raise(tok_.start, "Missing initializer in using declaration");
    } else if (!d->id->is<Identifier>() && !forInOf) {
      raise(tok_.start, "Complex binding patterns require an initialization value");
    }
    finish(d);
    node->declarations.push_back(d);
    if (kind == VariableKind::Var) addVarDeclarator(d);
    if (!eat(T::Comma)) break;
  }
  finish(node);
  if (kind != VariableKind::Var) addLexicalDeclaration(node, std::move(names), kind != VariableKind::Let);
  return node;
}

// ---------------------------------------------------------------------------------------------------
// Functions

namespace {

bool containsExpression(const Node* n) {
  if (!n) return false;
  switch (n->type) {
    case NodeType::AssignmentPattern:
      return true;
    case NodeType::ArrayPattern:
      for (const Node* e : static_cast<const ArrayPattern*>(n)->elements)
        if (containsExpression(e)) return true;
      return false;
    case NodeType::ObjectPattern:
      for (const Node* p : static_cast<const ObjectPattern*>(n)->properties) {
        if (p->is<Property>()) {
          auto* prop = static_cast<const Property*>(p);
          if (prop->computed || containsExpression(prop->value)) return true;
        } else if (containsExpression(p)) {
          return true;
        }
      }
      return false;
    case NodeType::RestElement:
      return containsExpression(static_cast<const RestElement*>(n)->argument);
    default:
      return false;
  }
}

}  // namespace

// Parameter shape and BoundNames; declares the names in the function's root scope.
static void computeParamInfo(Function* fn) {
  fn->hasSimpleParameterList = true;
  fn->hasParameterExpressions = false;
  fn->expectedArgumentCount = 0;
  bool counting = true;
  for (Node* p : fn->params) {
    if (!p->is<Identifier>()) fn->hasSimpleParameterList = false;
    if (containsExpression(p)) fn->hasParameterExpressions = true;
    if (p->is<AssignmentPattern>() || p->is<RestElement>()) counting = false;
    if (counting) fn->expectedArgumentCount++;
  }
}

void Parser::checkParams(Function* fn, bool allowDuplicates) {
  std::unordered_set<std::u16string> seen;
  for (const std::u16string& name : fn->parameterNames) {
    if (ctx_->strict) {
      if (name == u"eval" || name == u"arguments") raise(fn->start, "Unexpected eval or arguments in strict mode");
      K kw = keywordOf(name);
      if (isStrictReserved(kw)) raise(fn->start, "Unexpected strict mode reserved word '" + utf8(name) + "'");
    }
    if (!seen.insert(name).second) {
      if (!allowDuplicates) raise(fn->start, "Duplicate parameter name not allowed in this context");
      fn->hasDuplicateParameters = true;
    }
  }
}

void Parser::parseFunctionParams(Function* fn) {
  expect(T::LParen);
  fn->params = parseBindingList(T::RParen, false, true);
  checkYieldAwaitInDefaultParams();
  computeParamInfo(fn);
  std::vector<std::u16string> names;
  std::vector<std::u16string>* saved = collect_;
  collect_ = &names;
  for (Node* p : fn->params) checkLValInnerPattern(p, Bind::Param);
  collect_ = saved;
  for (auto& name : names) ctx_->root->params.insert(name);
  fn->parameterNames = std::move(names);
}

void Parser::parseFunctionBody(Function* fn, bool isArrow, bool isMethod, bool noIn) {
  if (isArrow && !is(T::LBrace)) {
    fn->body = parseMaybeAssign(noIn);
    fn->expression = true;
    checkParams(fn, false);
    return;
  }
  auto* body = startNode<BlockStatement>();
  body->isFunctionBody = true;
  expect(T::LBrace);
  bool becameStrict = false;
  parseDirectives(body->body, fn, &becameStrict);
  checkParams(fn, !ctx_->strict && !isArrow && !isMethod && fn->hasSimpleParameterList);
  if (ctx_->strict && fn->id) {
    const std::u16string& name = fn->id->name;
    if (name == u"eval" || name == u"arguments") raise(fn->id->start, "Unexpected eval or arguments in strict mode");
    if (isStrictReserved(keywordOf(name))) raise(fn->id->start, "Unexpected strict mode reserved word");
  }
  while (!is(T::RBrace)) {
    if (is(T::EOS)) unexpected();
    body->body.push_back(parseStatement(nullptr, false));
  }
  next();
  fn->body = finish(body);
}

// `function` is the current token (after `async`, if any). context: as for parseStatement.
Node* Parser::parseFunctionStatement(Pos start, bool isAsync, bool declarationPosition, bool exportDefault) {
  auto* fn = startNode<FunctionDeclaration>(start);
  next();
  parseFunction(fn, true, exportDefault, isAsync, !declarationPosition);
  return fn;
}

Function* Parser::parseFunction(Function* node, bool statement, bool nullableId, bool isAsync, bool hanging) {
  node->async = isAsync;
  node->generator = eat(T::Star);
  if (hanging && node->generator) unexpected();
  if (statement) {
    if (nullableId && !isName()) {
      node->id = nullptr;
    } else {
      if (!isName()) unexpected();
      node->id = parseIdent(false);
      if (ctx_->strict && (node->id->name == u"eval" || node->id->name == u"arguments"))
        raise(node->id->start, "Unexpected eval or arguments in strict mode");
    }
  }
  uint32_t oldYield = ctx_->yieldPos, oldAwait = ctx_->awaitPos, oldAwaitIdent = ctx_->awaitIdentPos;
  FnCtx* c = enterFn(FnCtx::Kind::Function, isAsync, node->generator, node);
  if (!statement && isName()) {
    node->id = parseIdent(false);
    if (ctx_->strict && (node->id->name == u"eval" || node->id->name == u"arguments"))
      raise(node->id->start, "Unexpected eval or arguments in strict mode");
  }
  parseFunctionParams(node);
  parseFunctionBody(node, false, false, false);
  finishFunctionAnnotations(node, c);
  exitFn();
  ctx_->yieldPos = oldYield;
  ctx_->awaitPos = oldAwait;
  ctx_->awaitIdentPos = oldAwaitIdent;
  finish(node);
  node->sourceText = sliceOf(source_, node);
  if (statement) {
    auto* decl = static_cast<FunctionDeclaration*>(node);
    if (node->id) {
      declareFunction(decl, node->id->start);
    } else {
      // export default function () {}
      declareLexicalName(u"*default*", Lex::OtherFunction, node, node->start);
      addLexicalDeclaration(node, {u"*default*"}, false);
    }
  }
  return node;
}

// ---------------------------------------------------------------------------------------------------
// Control flow

Node* Parser::parseIf(Pos start) {
  auto* node = startNode<IfStatement>(start);
  next();
  expect(T::LParen);
  node->test = parseExpression();
  expect(T::RParen);
  auto clause = [&](BlockScope& implicit) -> Node* {
    if (isKw(K::Function) && !ctx_->strict) {
      // Annex B.3.3: as if wrapped in a block.
      enterScope(Scope::Kind::Block, &implicit.lexicallyScopedDeclarations);
      Node* fn = parseStatement("if", false);
      scope_->annexB.push_back(fn->as<FunctionDeclaration>());
      exitScope();
      return fn;
    }
    return parseStatement("if", false);
  };
  node->consequent = clause(node->consequentScope);
  if (eatKw(K::Else)) node->alternate = clause(node->alternateScope);
  return finish(node);
}

Node* Parser::parseWhile(Pos start) {
  auto* node = startNode<WhileStatement>(start);
  next();
  expect(T::LParen);
  node->test = parseExpression();
  expect(T::RParen);
  ctx_->labels.push_back({u"", 1, 0});
  node->body = parseStatement("while", false);
  ctx_->labels.pop_back();
  return finish(node);
}

Node* Parser::parseDoWhile(Pos start) {
  auto* node = startNode<DoWhileStatement>(start);
  next();
  ctx_->labels.push_back({u"", 1, 0});
  node->body = parseStatement("do", false);
  ctx_->labels.pop_back();
  expectKw(K::While);
  expect(T::LParen);
  node->test = parseExpression();
  expect(T::RParen);
  eat(T::Semi);
  return finish(node);
}

Node* Parser::parseReturn(Pos start) {
  if (!ctx_->allowReturn) raise(tok_.start, "Illegal return statement");
  auto* node = startNode<ReturnStatement>(start);
  next();
  if (!eat(T::Semi) && !canInsertSemicolon()) {
    node->argument = parseExpression();
    semicolon();
  }
  return finish(node);
}

Node* Parser::parseThrow(Pos start) {
  auto* node = startNode<ThrowStatement>(start);
  next();
  if (tok_.nlBefore) raise(prevEnd_, "Illegal newline after throw");
  node->argument = parseExpression();
  semicolon();
  return finish(node);
}

Node* Parser::parseWith(Pos start) {
  if (ctx_->strict) raise(tok_.start, "Strict mode code may not include a with statement");
  auto* node = startNode<WithStatement>(start);
  next();
  expect(T::LParen);
  node->object = parseExpression();
  expect(T::RParen);
  node->body = parseStatement("with", false);
  return finish(node);
}

Node* Parser::parseBreakContinue(Pos start, bool isBreak) {
  Identifier* label = nullptr;
  next();
  if (!eat(T::Semi) && !canInsertSemicolon()) {
    if (!isName()) unexpected();
    label = parseIdent(false);
    semicolon();
  }
  size_t i = 0;
  auto& labels = ctx_->labels;
  for (; i < labels.size(); i++) {
    auto& lab = labels[i];
    if (!label || lab.name == label->name) {
      if (lab.kind != 0 && (isBreak || lab.kind == 1)) break;
      if (label && isBreak) break;
    }
  }
  if (i == labels.size()) raise(start.start, isBreak ? "Illegal break statement" : "Illegal continue statement");
  if (label && !isBreak && labels[i].kind != 1) raise(start.start, "Illegal continue statement");
  if (isBreak) {
    auto* node = startNode<BreakStatement>(start);
    node->label = label;
    return finish(node);
  }
  auto* node = startNode<ContinueStatement>(start);
  node->label = label;
  return finish(node);
}

