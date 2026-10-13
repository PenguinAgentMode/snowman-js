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
    // `for (using of ...)` is a for-of over the identifier `using`; `for (using of = x;;)` declares `of`.
    Lexer::State state = lexer_.save();
    Token t1, t2;
    lexer_.next(t1);
    lexer_.next(t2);
    lexer_.restore(state);
    return t2.type == T::Assign;
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
  checkStack();
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
void computeParamInfo(Function* fn) {
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
      // Annex B.3.2: a sloppy plain function declaration directly in a block is a var-hoisting candidate.
      if (!hanging && !ctx_->strict && !node->async && !node->generator && scope_->kind != Scope::Kind::Root)
        scope_->annexB.push_back(decl);
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

Node* Parser::parseLabeled(Pos start, Identifier* label, const char* context) {
  for (auto& lab : ctx_->labels)
    if (lab.name == label->name) raise(label->start, "Label '" + utf8(label->name) + "' has already been declared");
  uint8_t kind = 0;
  if (isKw(K::Do) || isKw(K::For) || isKw(K::While)) kind = 1;
  else if (isKw(K::Switch)) kind = 2;
  for (size_t i = ctx_->labels.size(); i-- > 0;) {
    auto& lab = ctx_->labels[i];
    if (lab.statementStart != start.start) break;
    lab.statementStart = tok_.start;
    lab.kind = kind;
  }
  ctx_->labels.push_back({label->name, kind, tok_.start});
  std::string bodyContext = context ? (hasContext(context, "label") ? context : std::string(context) + "label") : "label";
  auto* node = startNode<LabeledStatement>(start);
  node->label = label;
  node->body = parseStatement(bodyContext.c_str(), false);
  ctx_->labels.pop_back();
  return finish(node);
}

Node* Parser::parseFor(Pos start) {
  next();
  bool isAwait = false;
  uint32_t awaitAt = 0;
  if (isKw(K::Await) && canAwait()) {
    awaitAt = tok_.start;
    isAwait = true;
    if (ctx_->kind == FnCtx::Kind::Module) ctx_->usesTopLevelAwait = true;
    next();
  }
  ctx_->labels.push_back({u"", 1, 0});
  std::vector<LexicalDeclaration> headDecls;
  Scope* headScope = enterScope(Scope::Kind::For, &headDecls);
  expect(T::LParen);
  Node* result;
  if (is(T::Semi)) {
    if (isAwait) raise(awaitAt, "Unexpected 'await'");
    result = parseForInOf(start, nullptr, false, headScope);
  } else {
    bool let = isLet(nullptr);
    if (isKw(K::Var) || isKw(K::Const) || let) {
      Pos ds = here();
      VariableKind kind = let ? VariableKind::Let : isKw(K::Var) ? VariableKind::Var : VariableKind::Const;
      next();
      VariableDeclaration* init = parseVar(ds, kind, true, true);
      result = parseForInOf(start, init, isAwait, headScope);
    } else if (isUsing(true) || isAwaitUsing(true)) {
      Pos ds = here();
      bool awaitUsing = isKw(K::Await);
      if (awaitUsing) next();
      if (awaitUsing && ctx_->kind == FnCtx::Kind::Module) ctx_->usesTopLevelAwait = true;
      next();
      VariableDeclaration* init =
          parseVar(ds, awaitUsing ? VariableKind::AwaitUsing : VariableKind::Using, true, true);
      if (!isKw(K::Of)) {
        if (isKw(K::In)) unexpected();
      }
      result = parseForInOf(start, init, isAwait, headScope);
    } else {
      bool startsWithLet = isKw(K::Let);
      Token first = tok_;
      DestructuringErrors errors;
      Node* init;
      if (isAwait) {
        potentialArrowInForAwait_ = true;
        init = parseExprSubscripts(&errors, true);
        potentialArrowInForAwait_ = false;
      } else {
        init = parseExpression(true, &errors);
      }
      bool forOf = isKw(K::Of);
      if (isKw(K::In) || forOf) {
        if (isAwait && !forOf) raise(awaitAt, "Unexpected 'await'");
        if (forOf && !isAwait && first.type == T::Name && first.kw == K::Async && !first.escaped &&
            init->is<Identifier>() && !init->parenthesized && init->start == first.start)
          raise(init->start, "The left-hand side of a for-of loop may not be 'async'");
        if (startsWithLet && forOf) raise(init->start, "The left-hand side of a for-of loop may not start with let");
        if (ctx_->strict || !init->is<CallExpression>()) {
          init = toAssignable(init, false, &errors);
          checkLValPattern(init, Bind::None);
        }
      } else {
        checkExpressionErrors(&errors, true);
      }
      result = parseForInOf(start, init, isAwait, headScope);
    }
  }
  exitScope();
  ctx_->labels.pop_back();
  if (result->is<ForStatement>()) result->as<ForStatement>()->lexicallyScopedDeclarations = std::move(headDecls);
  else if (result->is<ForInStatement>()) result->as<ForInStatement>()->lexicallyScopedDeclarations = std::move(headDecls);
  else result->as<ForOfStatement>()->lexicallyScopedDeclarations = std::move(headDecls);
  return result;
}

// After the head's first part: `init` is null, a VariableDeclaration or a target; the current token is `;`,
// `in` or `of`.
Node* Parser::parseForInOf(Pos start, Node* init, bool isAwait, Scope* headScope) {
  (void)headScope;
  bool forIn = isKw(K::In), forOf = isKw(K::Of);
  if (init && (forIn || forOf)) {
    if (init->is<VariableDeclaration>()) {
      auto* decl = init->as<VariableDeclaration>();
      if (decl->declarations.size() != 1)
        raise(init->start, "Invalid left-hand side in for-" + std::string(forIn ? "in" : "of") + " loop: must have a single binding");
      VariableDeclarator* d = decl->declarations[0];
      if (d->init && (!forIn || ctx_->strict || decl->kind != VariableKind::Var || !d->id->is<Identifier>()))
        raise(init->start, std::string(forIn ? "for-in" : "for-of") + " loop variable declaration may not have an initializer");
      if (forIn && (decl->kind == VariableKind::Using || decl->kind == VariableKind::AwaitUsing)) unexpected();
    }
    if (isAwait && forIn) unexpected();
    next();
    Node* right = forIn ? parseExpression() : parseMaybeAssign();
    expect(T::RParen);
    Node* body = parseStatement("for", false);
    if (forIn) {
      auto* node = startNode<ForInStatement>(start);
      node->left = init;
      node->right = right;
      node->body = body;
      return finish(node);
    }
    auto* node = startNode<ForOfStatement>(start);
    node->left = init;
    node->right = right;
    node->body = body;
    node->await = isAwait;
    return finish(node);
  }
  if (isAwait) unexpected();
  if (init && init->is<VariableDeclaration>()) {
    auto* decl = init->as<VariableDeclaration>();
    if (decl->kind == VariableKind::Const || decl->kind == VariableKind::Using || decl->kind == VariableKind::AwaitUsing) {
      for (auto* d : decl->declarations)
        if (!d->init) raise(d->end, "Missing initializer in declaration");
    }
    for (auto* d : decl->declarations)
      if (!d->init && !d->id->is<Identifier>()) raise(d->end, "Complex binding patterns require an initialization value");
  }
  auto* node = startNode<ForStatement>(start);
  node->init = init;
  expect(T::Semi);
  if (!is(T::Semi)) node->test = parseExpression();
  expect(T::Semi);
  if (!is(T::RParen)) node->update = parseExpression();
  expect(T::RParen);
  node->body = parseStatement("for", false);
  return finish(node);
}

Node* Parser::parseSwitch(Pos start) {
  auto* node = startNode<SwitchStatement>(start);
  next();
  expect(T::LParen);
  node->discriminant = parseExpression();
  expect(T::RParen);
  expect(T::LBrace);
  ctx_->labels.push_back({u"", 2, 0});
  enterScope(Scope::Kind::Switch, &node->lexicallyScopedDeclarations);
  SwitchCase* cur = nullptr;
  bool sawDefault = false;
  while (!is(T::RBrace)) {
    if (isKw(K::Case) || isKw(K::Default)) {
      bool isCase = isKw(K::Case);
      if (cur) finish(cur);
      cur = startNode<SwitchCase>();
      node->cases.push_back(cur);
      next();
      if (isCase) {
        cur->test = parseExpression();
      } else {
        if (sawDefault) raise(prevStart_, "More than one default clause in switch statement");
        sawDefault = true;
      }
      expect(T::Colon);
    } else {
      if (!cur) unexpected();
      cur->consequent.push_back(parseStatement(nullptr, false));
    }
  }
  exitScope();
  if (cur) finish(cur);
  next();
  ctx_->labels.pop_back();
  return finish(node);
}

Node* Parser::parseTry(Pos start) {
  auto* node = startNode<TryStatement>(start);
  next();
  node->block = parseBlock(true);
  if (isKw(K::Catch)) {
    auto* clause = startNode<CatchClause>();
    next();
    if (eat(T::LParen)) {
      Node* param = parseBindingAtom();
      bool simple = param->is<Identifier>();
      enterScope(Scope::Kind::Catch, nullptr);
      std::vector<std::u16string>* saved = collect_;
      collect_ = &clause->paramNames;
      checkLValPattern(param, simple ? Bind::CatchSimple : Bind::CatchPattern);
      collect_ = saved;
      expect(T::RParen);
      clause->param = param;
    } else {
      enterScope(Scope::Kind::Catch, nullptr);
    }
    clause->body = parseBlock(false);
    exitScope();
    node->handler = finish(clause);
  }
  if (eatKw(K::Finally)) node->finalizer = parseBlock(true);
  if (!node->handler && !node->finalizer) raise(tok_.start, "Missing catch or finally after try");
  return finish(node);
}

// ---------------------------------------------------------------------------------------------------
// Classes

namespace {

bool keyIs(const Node* key, bool computed, const char16_t* name) {
  if (computed || !key) return false;
  if (key->is<Identifier>()) return static_cast<const Identifier*>(key)->name == name;
  if (key->is<Literal>()) {
    auto* lit = static_cast<const Literal*>(key);
    return lit->kind == LiteralKind::String && lit->string == name;
  }
  return false;
}

}  // namespace

Class* Parser::parseClass(Class* node, bool statement, bool nullableId) {
  next();  // class
  bool oldStrict = ctx_->strict;
  ctx_->strict = true;
  if (isName() && !isKw(K::Extends)) {
    node->id = parseIdent(false);
    if (node->id->name == u"eval" || node->id->name == u"arguments")
      raise(node->id->start, "Unexpected eval or arguments in strict mode");
  } else if (statement && !nullableId) {
    unexpected();
  }
  if (eatKw(K::Extends)) node->superClass = parseExprSubscripts(nullptr, false);
  enterPrivateScope();
  auto* body = startNode<ClassBody>();
  expect(T::LBrace);
  bool hadConstructor = false;
  while (!is(T::RBrace)) {
    if (is(T::EOS)) unexpected();
    Node* element = parseClassElement(node, hadConstructor);
    if (element) body->body.push_back(element);
  }
  next();
  node->body = finish(body);
  ctx_->strict = oldStrict;
  for (auto& [name, bits] : private_->declared) {
    (void)bits;
    node->privateBoundIdentifiers.push_back(name);
  }
  std::sort(node->privateBoundIdentifiers.begin(), node->privateBoundIdentifiers.end());
  exitPrivateScope();
  finish(node);
  node->sourceText = sliceOf(source_, node);
  if (statement) {
    std::u16string name = node->id ? node->id->name : std::u16string(u"*default*");
    declareLexicalName(name, Lex::Class, node, node->id ? node->id->start : node->start);
    addLexicalDeclaration(node, {name}, false);
  }
  return node;
}

Node* Parser::parseClassElement(Class* cls, bool& hadConstructor) {
  if (eat(T::Semi)) return nullptr;
  Pos start = here();
  Pos methodStart = start;
  auto isElementNameStart = [&]() {
    return is(T::Name) || is(T::PrivateName) || is(T::Num) || is(T::BigInt) || is(T::Str) || is(T::LBracket);
  };
  std::u16string keyName;
  Token keyTok;
  bool isStatic = false, isAsync = false, isGenerator = false;
  MethodKind kind = MethodKind::Method;
  if (isKw(K::Static)) {
    keyTok = tok_;
    next();
    if (is(T::LBrace)) {
      // static { ... }
      auto* block = startNode<StaticBlock>(start);
      next();
      uint32_t oldYield = ctx_->yieldPos, oldAwait = ctx_->awaitPos, oldAwaitIdent = ctx_->awaitIdentPos;
      FnCtx* c = enterFn(FnCtx::Kind::StaticBlock, false, false, block);
      while (!is(T::RBrace)) {
        if (is(T::EOS)) unexpected();
        block->body.push_back(parseStatement(nullptr, false));
      }
      next();
      finishVarScope(block, c);
      exitFn();
      ctx_->yieldPos = oldYield;
      ctx_->awaitPos = oldAwait;
      ctx_->awaitIdentPos = oldAwaitIdent;
      return finish(block);
    }
    if (isElementNameStart() || is(T::Star)) {
      isStatic = true;
      methodStart = here();
    } else {
      keyName = u"static";
    }
  }
  if (keyName.empty() && isKw(K::Async)) {
    keyTok = tok_;
    next();
    if ((isElementNameStart() || is(T::Star)) && !tok_.nlBefore) isAsync = true;
    else keyName = u"async";
  }
  if (keyName.empty() && eat(T::Star)) isGenerator = true;
  if (keyName.empty() && !isAsync && !isGenerator && (isKw(K::Get) || isKw(K::Set))) {
    keyTok = tok_;
    bool getter = isKw(K::Get);
    next();
    if (isElementNameStart()) kind = getter ? MethodKind::Get : MethodKind::Set;
    else keyName = getter ? u"get" : u"set";
  }
  Node* key;
  bool computed = false;
  if (!keyName.empty()) {
    auto* id = startNode<Identifier>({keyTok.start, keyTok.line, keyTok.col});
    id->name = keyName;
    id->end = keyTok.end;
    key = id;
  } else if (is(T::PrivateName)) {
    if (tok_.str == u"constructor") raise(tok_.start, "Classes may not have a private field named '#constructor'");
    key = parsePrivateIdent();
  } else {
    key = parsePropertyName(computed);
  }
  bool isPrivate = key->is<PrivateIdentifier>();
  if (is(T::LParen) || kind != MethodKind::Method || isGenerator || isAsync) {
    bool isConstructor = !isStatic && keyIs(key, computed, u"constructor");
    if (isConstructor && kind != MethodKind::Method) raise(key->start, "Class constructor may not be an accessor");
    if (isConstructor && isGenerator) raise(key->start, "Class constructor may not be a generator");
    if (isConstructor && isAsync) raise(key->start, "Class constructor may not be an async method");
    if (isStatic && keyIs(key, computed, u"prototype"))
      raise(key->start, "Classes may not have a static property named 'prototype'");
    auto* method = startNode<MethodDefinition>(start);
    method->key = key;
    method->computed = computed;
    method->isStatic = isStatic;
    method->kind = isConstructor ? MethodKind::Constructor : kind;
    FunctionKind fk = isConstructor ? (cls->superClass ? FunctionKind::DerivedConstructor : FunctionKind::ClassConstructor)
                      : kind == MethodKind::Get ? FunctionKind::Getter
                      : kind == MethodKind::Set ? FunctionKind::Setter
                                                : FunctionKind::Method;
    method->value = parseMethod(methodStart, isGenerator, isAsync, fk, isConstructor && cls->superClass);
    if (kind == MethodKind::Get && !method->value->params.empty())
      raise(method->value->start, "Getter must not have any formal parameters");
    if (kind == MethodKind::Set && method->value->params.size() != 1)
      raise(method->value->start, "Setter must have exactly one formal parameter");
    if (kind == MethodKind::Set && method->value->params[0]->is<RestElement>())
      raise(method->value->start, "Setter function argument must not be a rest parameter");
    if (isConstructor) {
      if (hadConstructor) raise(key->start, "A class may only have one constructor");
      hadConstructor = true;
      cls->constructor = method;
    }
    if (isPrivate) {
      uint8_t bits = (kind == MethodKind::Get ? 1 : kind == MethodKind::Set ? 2 : 4) | (isStatic ? 8 : 16);
      auto& name = key->as<PrivateIdentifier>()->name;
      auto it = private_->declared.find(name);
      if (it == private_->declared.end()) {
        private_->declared[name] = bits;
      } else {
        uint8_t prev = it->second;
        bool ok = (prev & 3) && (bits & 3) && !(prev & bits & 3) && !(prev & 4) && (prev & 24) == (bits & 24);
        if (!ok) raise(key->start, "Identifier '#" + utf8(name) + "' has already been declared");
        it->second |= bits;
      }
    }
    return finish(method);
  }
  if (keyIs(key, computed, u"constructor")) raise(key->start, "Classes may not have a field named 'constructor'");
  if (isStatic && keyIs(key, computed, u"prototype"))
    raise(key->start, "Classes may not have a static property named 'prototype'");
  auto* field = startNode<PropertyDefinition>(start);
  field->key = key;
  field->computed = computed;
  field->isStatic = isStatic;
  if (eat(T::Assign)) {
    uint32_t oldYield = ctx_->yieldPos, oldAwait = ctx_->awaitPos, oldAwaitIdent = ctx_->awaitIdentPos;
    FnCtx* c = enterFn(FnCtx::Kind::FieldInit, false, false, nullptr);
    field->value = parseMaybeAssign();
    field->hasDirectEval = c->evalCount > 0;
    exitFn();
    ctx_->yieldPos = oldYield;
    ctx_->awaitPos = oldAwait;
    ctx_->awaitIdentPos = oldAwaitIdent;
  }
  semicolon();
  if (isPrivate) {
    auto& name = key->as<PrivateIdentifier>()->name;
    if (private_->declared.count(name)) raise(key->start, "Identifier '#" + utf8(name) + "' has already been declared");
    private_->declared[name] = 4 | (isStatic ? 8 : 16);
  }
  return finish(field);
}

FunctionExpression* Parser::parseMethod(Pos sourceStart, bool isGenerator, bool isAsync, FunctionKind kind,
                                        bool allowSuperCall) {
  auto* fn = startNode<FunctionExpression>();
  fn->generator = isGenerator;
  fn->async = isAsync;
  fn->kind = kind;
  uint32_t oldYield = ctx_->yieldPos, oldAwait = ctx_->awaitPos, oldAwaitIdent = ctx_->awaitIdentPos;
  FnCtx* c = enterFn(FnCtx::Kind::Method, isAsync, isGenerator, fn);
  c->allowSuperCall = allowSuperCall;
  parseFunctionParams(fn);
  parseFunctionBody(fn, false, true, false);
  finishFunctionAnnotations(fn, c);
  exitFn();
  ctx_->yieldPos = oldYield;
  ctx_->awaitPos = oldAwait;
  ctx_->awaitIdentPos = oldAwaitIdent;
  finish(fn);
  fn->sourceText = std::u16string_view(source_).substr(sourceStart.start, fn->end - sourceStart.start);
  return fn;
}

// ---------------------------------------------------------------------------------------------------
// Modules

namespace {

bool isWellFormed(const std::u16string& s) {
  for (size_t i = 0; i < s.size(); i++) {
    char16_t c = s[i];
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
        i++;
        continue;
      }
      return false;
    }
    if (c >= 0xDC00 && c <= 0xDFFF) return false;
  }
  return true;
}

}  // namespace

std::u16string Parser::moduleExportNameString(Node* n) {
  if (n->is<Identifier>()) return n->as<Identifier>()->name;
  return n->as<Literal>()->string;
}

void Parser::addExportName(const std::u16string& name, uint32_t pos) {
  if (!exportedNames_.insert(name).second) raise(pos, "Duplicate export of '" + utf8(name) + "'");
}

// ModuleExportName: IdentifierName or a well-formed string literal.
Node* Parser::parseModuleExportName() {
  if (is(T::Str)) {
    Literal* lit = parseLiteralToken();
    if (!isWellFormed(lit->string)) raise(lit->start, "Export name is not a well-formed Unicode string");
    return lit;
  }
  return parseIdent(true);
}

Literal* Parser::parseModuleSpecifier() {
  if (!is(T::Str)) unexpected();
  return parseLiteralToken();
}

std::vector<ImportAttribute*> Parser::parseWithClause() {
  std::vector<ImportAttribute*> attributes;
  if (!isKw(K::With)) return attributes;
  next();
  expect(T::LBrace);
  std::unordered_set<std::u16string> keys;
  while (!eat(T::RBrace)) {
    auto* attr = startNode<ImportAttribute>();
    attr->key = is(T::Str) ? static_cast<Node*>(parseLiteralToken()) : parseIdent(true);
    std::u16string key = moduleExportNameString(attr->key);
    if (!keys.insert(key).second) raise(attr->key->start, "Duplicate import attribute '" + utf8(key) + "'");
    expect(T::Colon);
    if (!is(T::Str)) unexpected();
    attr->value = parseLiteralToken();
    attributes.push_back(finish(attr));
    if (!is(T::RBrace)) expect(T::Comma);
  }
  return attributes;
}

uint32_t Parser::addModuleRequest(Literal* source, const std::vector<ImportAttribute*>& attributes, ImportPhase phase) {
  ModuleRequest request;
  request.specifier = source->string;
  request.phase = phase;
  for (ImportAttribute* a : attributes) request.attributes.push_back({moduleExportNameString(a->key), a->value->string});
  auto& list = program_->requestedModules;
  // ModuleRequests: one entry per (ModuleRequestsKeyEqual, [[Phase]]).
  for (uint32_t i = 0; i < list.size(); i++) {
    if (list[i].specifier != request.specifier || list[i].attributes.size() != request.attributes.size() ||
        list[i].phase != phase) {
      continue;
    }
    bool same = true;
    for (auto& a : request.attributes) {
      bool found = false;
      for (auto& b : list[i].attributes) found = found || (a.key == b.key && a.value == b.value);
      same = same && found;
    }
    if (same) return i;
  }
  list.push_back(std::move(request));
  return static_cast<uint32_t>(list.size() - 1);
}

bool Parser::isSourcePhaseImport() {
  // `import source x from "m"`, and `import source from from "m"` (binding `from`); `import source from "m"` and
  // `import source, ...` import a default binding named source.
  auto [first, second] = peek2();
  if (first.type != T::Name) return false;
  if (first.str == u"from" && !first.escaped) return second.type == T::Name && second.str == u"from" && !second.escaped;
  return true;
}

Node* Parser::parseImport(Pos start) {
  auto* node = startNode<ImportDeclaration>(start);
  next();  // import
  std::vector<ImportEntry> entries;
  auto bindLocal = [&](Identifier* local) {
    checkLValSimple(local, Bind::Import);
  };
  if (isContextual(u"defer") && peek().type == T::Star) {
    // import defer NameSpaceImport FromClause WithClause?
    node->phase = ImportPhase::Defer;
    next();
    auto* spec = startNode<ImportNamespaceSpecifier>();
    next();  // *
    expectKw(K::As);
    spec->local = parseIdent(false);
    bindLocal(spec->local);
    node->specifiers.push_back(finish(spec));
    entries.push_back({0, true, false, u"", spec->local->name, spec});
    expectKw(K::From);
  } else if (isContextual(u"source") && isSourcePhaseImport()) {
    // import source ImportedBinding FromClause WithClause?
    node->phase = ImportPhase::Source;
    next();
    auto* spec = startNode<ImportDefaultSpecifier>();
    spec->local = parseIdent(false);
    bindLocal(spec->local);
    node->specifiers.push_back(finish(spec));
    entries.push_back({0, false, true, u"", spec->local->name, spec});
    expectKw(K::From);
  } else if (!is(T::Str)) {
    if (isName()) {
      auto* spec = startNode<ImportDefaultSpecifier>();
      spec->local = parseIdent(false);
      bindLocal(spec->local);
      node->specifiers.push_back(finish(spec));
      entries.push_back({0, false, false, u"default", spec->local->name, spec});
      if (!eat(T::Comma)) goto from;
    }
    if (is(T::Star)) {
      auto* spec = startNode<ImportNamespaceSpecifier>();
      next();
      expectKw(K::As);
      spec->local = parseIdent(false);
      bindLocal(spec->local);
      node->specifiers.push_back(finish(spec));
      entries.push_back({0, true, false, u"", spec->local->name, spec});
    } else {
      expect(T::LBrace);
      while (!eat(T::RBrace)) {
        auto* spec = startNode<ImportSpecifier>();
        Token importedTok = tok_;
        spec->imported = parseModuleExportName();
        if (eatKw(K::As)) {
          spec->local = parseIdent(false);
        } else {
          if (!spec->imported->is<Identifier>()) unexpected();
          checkUnreserved(importedTok);
          auto* local = startAt<Identifier>(spec->imported);
          local->name = spec->imported->as<Identifier>()->name;
          local->end = spec->imported->end;
          spec->local = local;
        }
        bindLocal(spec->local);
        node->specifiers.push_back(finish(spec));
        entries.push_back({0, false, false, moduleExportNameString(spec->imported), spec->local->name, spec});
        if (!is(T::RBrace)) expect(T::Comma);
      }
    }
  from:
    if (!isKw(K::From)) unexpected();
    next();
  }
  node->source = parseModuleSpecifier();
  node->attributes = parseWithClause();
  semicolon();
  uint32_t request = addModuleRequest(node->source, node->attributes, node->phase);
  for (auto& e : entries) {
    e.moduleRequest = request;
    program_->importEntries.push_back(std::move(e));
  }
  return finish(node);
}

Node* Parser::parseExport(Pos start) {
  next();  // export
  auto localEntry = [&](const std::u16string& exportName, const std::u16string& localName, Node* n) {
    ExportEntry e;
    e.exportName = exportName;
    e.localName = localName;
    e.node = n;
    program_->localExportEntries.push_back(std::move(e));
  };
  // export * from "m" / export * as ns from "m"
  if (is(T::Star)) {
    auto* node = startNode<ExportAllDeclaration>(start);
    next();
    if (eatKw(K::As)) {
      node->exported = parseModuleExportName();
      addExportName(moduleExportNameString(node->exported), node->exported->start);
    }
    expectKw(K::From);
    node->source = parseModuleSpecifier();
    node->attributes = parseWithClause();
    semicolon();
    uint32_t request = addModuleRequest(node->source, node->attributes, ImportPhase::Evaluation);
    ExportEntry e;
    e.moduleRequest = request;
    e.node = node;
    if (node->exported) {
      e.exportName = moduleExportNameString(node->exported);
      e.importKind = ExportEntry::ImportKind::All;
      program_->indirectExportEntries.push_back(std::move(e));
    } else {
      e.importKind = ExportEntry::ImportKind::AllButDefault;
      program_->starExportEntries.push_back(std::move(e));
    }
    return finish(node);
  }
  // export default ...
  if (isKw(K::Default)) {
    auto* node = startNode<ExportDefaultDeclaration>(start);
    next();
    addExportName(u"default", prevStart_);
    Pos ds = here();
    if (isKw(K::Function) || isAsyncFunction()) {
      bool isAsync = isKw(K::Async);
      if (isAsync) next();
      auto* fn = static_cast<FunctionDeclaration*>(parseFunctionStatement(ds, isAsync, true, true));
      node->declaration = fn;
      localEntry(u"default", fn->id ? fn->id->name : u"*default*", node);
    } else if (isKw(K::Class)) {
      auto* cls = parseClass(startNode<ClassDeclaration>(ds), true, true);
      node->declaration = cls;
      localEntry(u"default", cls->id ? cls->id->name : u"*default*", node);
    } else {
      node->declaration = parseMaybeAssign();
      semicolon();
      declareLexicalName(u"*default*", Lex::Let, node, node->start);
      addLexicalDeclaration(node, {u"*default*"}, false);
      localEntry(u"default", u"*default*", node);
    }
    return finish(node);
  }
  auto* node = startNode<ExportNamedDeclaration>(start);
  // export var / let / const / function / class
  if (isKw(K::Var) || isKw(K::Const) || isLet(nullptr) || isKw(K::Function) || isKw(K::Class) || isAsyncFunction() ||
      isUsing(false) || isAwaitUsing(false)) {
    if (isUsing(false) || isAwaitUsing(false)) unexpected();
    size_t lexBefore = scope_->annotation ? scope_->annotation->size() : 0;
    size_t varBefore = program_->varScopedDeclarations.size();
    node->declaration = parseStatement(nullptr, true);
    std::vector<std::u16string> names;
    if (node->declaration->is<VariableDeclaration>() && node->declaration->as<VariableDeclaration>()->kind == VariableKind::Var) {
      for (size_t i = varBefore; i < program_->varScopedDeclarations.size(); i++) {
        std::vector<std::u16string> declNames;
        std::vector<std::u16string>* saved = collect_;
        collect_ = &declNames;
        checkLValPattern(program_->varScopedDeclarations[i]->as<VariableDeclarator>()->id, Bind::Outside);
        collect_ = saved;
        names.insert(names.end(), declNames.begin(), declNames.end());
      }
    } else if (scope_->annotation) {
      for (size_t i = lexBefore; i < scope_->annotation->size(); i++)
        names.insert(names.end(), (*scope_->annotation)[i].boundNames.begin(), (*scope_->annotation)[i].boundNames.end());
    }
    for (auto& name : names) {
      addExportName(name, node->declaration->start);
      localEntry(name, name, node);
    }
    return finish(node);
  }
  // export { ... } [from "m"]
  expect(T::LBrace);
  std::vector<Token> localTokens;
  while (!eat(T::RBrace)) {
    auto* spec = startNode<ExportSpecifier>();
    localTokens.push_back(tok_);
    spec->local = parseModuleExportName();
    if (eatKw(K::As)) {
      spec->exported = parseModuleExportName();
    } else {
      spec->exported = spec->local;
    }
    addExportName(moduleExportNameString(spec->exported), spec->exported->start);
    node->specifiers.push_back(finish(spec));
    if (!is(T::RBrace)) expect(T::Comma);
  }
  if (isKw(K::From)) {
    next();
    node->source = parseModuleSpecifier();
    node->attributes = parseWithClause();
    uint32_t request = addModuleRequest(node->source, node->attributes, ImportPhase::Evaluation);
    for (auto* spec : node->specifiers) {
      ExportEntry e;
      e.exportName = moduleExportNameString(spec->exported);
      e.moduleRequest = request;
      e.importKind = ExportEntry::ImportKind::Name;
      e.importName = moduleExportNameString(spec->local);
      e.node = spec;
      program_->indirectExportEntries.push_back(std::move(e));
    }
  } else {
    for (size_t i = 0; i < node->specifiers.size(); i++) {
      auto* spec = node->specifiers[i];
      if (!spec->local->is<Identifier>()) raise(spec->local->start, "A string literal cannot be used as an exported binding without 'from'");
      checkUnreserved(localTokens[i]);
      const std::u16string& local = spec->local->as<Identifier>()->name;
      exportedBindings_.emplace_back(local, spec->local->start);
      localEntry(moduleExportNameString(spec->exported), local, spec);
    }
  }
  semicolon();
  return finish(node);
}

// Module-level early errors that need the whole module, and the import-to-indirect export rewrite
// (ParseModule steps 10-12).
void Parser::finishModule(Program* program) {
  Scope* root = ctx_->root;
  for (auto& [name, pos] : exportedBindings_) {
    if (!root->vars.count(name) && !root->lexical.count(name) && !root->functions.count(name))
      raise(pos, "Export '" + utf8(name) + "' is not defined in module");
  }
  std::vector<ExportEntry> locals;
  for (auto& e : program->localExportEntries) {
    const ImportEntry* ie = nullptr;
    for (auto& i : program->importEntries)
      if (e.localName && i.localName == *e.localName) ie = &i;
    if (!ie) {
      locals.push_back(std::move(e));
      continue;
    }
    // A re-export of an import; `import * as ns from "m"; export {ns}` re-exports m's namespace like
    // `export * as ns from "m"`.
    ExportEntry indirect;
    indirect.exportName = e.exportName;
    indirect.moduleRequest = ie->moduleRequest;
    indirect.importKind = ie->isNamespaceObject ? ExportEntry::ImportKind::All
                          : ie->isSource         ? ExportEntry::ImportKind::Source
                                                 : ExportEntry::ImportKind::Name;
    indirect.importName = ie->importName;
    indirect.node = e.node;
    program->indirectExportEntries.push_back(std::move(indirect));
  }
  program->localExportEntries = std::move(locals);
}

}  // namespace snowman::parser
