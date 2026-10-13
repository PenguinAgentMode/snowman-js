// Parser plumbing: tokens, function contexts, scopes and declarations, private names, identifiers.
#include <algorithm>

#include "parser/parser_impl.h"

namespace snowman::parser {

namespace {

Program* newProgram(std::u16string source) {
  Program* p = make<Program>();
  p->source = std::move(source);
  return p;
}

std::string utf8(std::u16string_view s) { return utf16ToUtf8(s); }

}  // namespace

std::u16string_view sliceOf(const std::u16string& source, const Node* n) {
  return std::u16string_view(source).substr(n->start, n->end - n->start);
}

Parser::Parser(std::u16string source, const ParseOptions& options)
    : program_(newProgram(std::move(source))),
      options_(options),
      source_(program_->source),
      lexer_(program_->source.data(), static_cast<uint32_t>(program_->source.size()),
             options.goal == SourceType::Module, true),
      module_(options.goal == SourceType::Module) {}

// ---------------------------------------------------------------------------------------------------
// Tokens

void Parser::next() {
  prevEnd_ = tok_.end;
  prevStart_ = tok_.start;
  lexer_.next(tok_);
}

Token Parser::peek() {
  Lexer::State state = lexer_.save();
  Token t;
  lexer_.next(t);
  lexer_.restore(state);
  return t;
}

std::pair<Token, Token> Parser::peek2() {
  Lexer::State state = lexer_.save();
  std::pair<Token, Token> t;
  lexer_.next(t.first);
  lexer_.next(t.second);
  lexer_.restore(state);
  return t;
}

bool Parser::eat(T t) {
  if (tok_.type != t) return false;
  next();
  return true;
}

bool Parser::eatKw(K k) {
  if (!isKw(k)) return false;
  next();
  return true;
}

void Parser::expect(T t) {
  if (!eat(t)) unexpected();
}

void Parser::expectKw(K k) {
  if (!eatKw(k)) unexpected();
}

bool Parser::canInsertSemicolon() const { return tok_.type == T::EOS || tok_.type == T::RBrace || tok_.nlBefore; }

void Parser::semicolon() {
  if (!eat(T::Semi) && !canInsertSemicolon()) unexpected();
}

void Parser::raise(uint32_t pos, const std::string& message) { lexer_.fail(pos, message); }

const char* const* stackLimit = nullptr;

void Parser::checkStack() {
  if (stackLimit && *stackLimit && static_cast<const char*>(__builtin_frame_address(0)) < *stackLimit)
    raise(tok_.start, "Nesting too deep (out of stack)");
}

void Parser::unexpectedAt(const Token& t) {
  if (t.type == T::EOS) raise(t.start, "Unexpected end of input");
  std::u16string_view text = std::u16string_view(source_).substr(t.start, std::min<uint32_t>(t.end - t.start, 40));
  raise(t.start, "Unexpected token '" + utf8(text) + "'");
}

void Parser::unexpected() { unexpectedAt(tok_); }

// ---------------------------------------------------------------------------------------------------
// Function contexts

FnCtx* Parser::enterFn(FnCtx::Kind kind, bool async, bool generator, VarScope* vs) {
  auto owned = std::make_unique<FnCtx>();
  FnCtx* c = owned.get();
  ctxPool_.push_back(std::move(owned));
  FnCtx* parent = ctx_;
  c->parent = parent;
  c->kind = kind;
  c->async = async;
  c->generator = generator;
  c->vs = vs;
  c->strict = parent ? parent->strict : (module_ || options_.strict);
  switch (kind) {
    case FnCtx::Kind::Script:
    case FnCtx::Kind::Module:
      c->allowNewTarget = options_.inFunction;
      c->allowSuperProperty = options_.inMethod;
      c->allowSuperCall = options_.inDerivedConstructor;
      c->allowArguments = !options_.inClassFieldInitializer;
      break;
    case FnCtx::Kind::Function:
    case FnCtx::Kind::Method:
      c->allowReturn = true;
      c->allowNewTarget = true;
      c->allowSuperProperty = kind == FnCtx::Kind::Method;
      break;
    case FnCtx::Kind::Arrow:
      c->allowReturn = true;
      c->allowNewTarget = parent->allowNewTarget;
      c->allowSuperProperty = parent->allowSuperProperty;
      c->allowSuperCall = parent->allowSuperCall;
      c->allowArguments = parent->allowArguments;
      break;
    case FnCtx::Kind::FieldInit:
      c->allowNewTarget = true;
      c->allowSuperProperty = true;
      c->allowArguments = false;
      break;
    case FnCtx::Kind::StaticBlock:
      c->allowNewTarget = true;
      c->allowSuperProperty = true;
      c->allowArguments = false;
      c->inStaticBlock = true;
      break;
  }
  ctx_ = c;
  Scope* root = enterScope(Scope::Kind::Root, vs ? &vs->lexicallyScopedDeclarations : nullptr);
  root->moduleRoot = kind == FnCtx::Kind::Module;
  root->functionRoot = kind != FnCtx::Kind::Script && kind != FnCtx::Kind::Module;
  c->root = root;
  return c;
}

void Parser::exitFn() {
  while (scope_ != ctx_->root) exitScope();
  exitScope();
  ctx_ = ctx_->parent;
}

bool Parser::awaitIsKeyword() const { return module_ || ctx_->async || ctx_->inStaticBlock; }

bool Parser::canAwait() const { return ctx_->async || ctx_->kind == FnCtx::Kind::Module; }

void Parser::markDirectEval() {
  for (FnCtx* c = ctx_; c; c = c->parent) {
    c->evalCount++;
    if (c->kind != FnCtx::Kind::Arrow) break;
  }
}

void Parser::markArguments() {
  if (!ctx_->allowArguments) raise(tok_.start, "'arguments' is not allowed in class field initializer or static initialization block");
  for (FnCtx* c = ctx_; c; c = c->parent) {
    c->argumentsCount++;
    if (c->kind != FnCtx::Kind::Arrow) break;
  }
}

// ---------------------------------------------------------------------------------------------------
// Scopes and declarations

Scope* Parser::enterScope(Scope::Kind kind, std::vector<LexicalDeclaration>* annotation) {
  Scope* s;
  if (kind != Scope::Kind::Root && !freeScopes_.empty()) {
    s = freeScopes_.back();
    freeScopes_.pop_back();
  } else {
    auto owned = std::make_unique<Scope>();
    s = owned.get();
    scopePool_.push_back(std::move(owned));
  }
  s->kind = kind;
  s->parent = scope_;
  s->annotation = annotation;
  scope_ = s;
  return s;
}

void Parser::exitScope() {
  Scope* s = scope_;
  if (s->kind == Scope::Kind::Root) {
    for (FunctionDeclaration* f : s->annexB) {
      const std::u16string& name = f->id->name;
      if (s->lexical.count(name)) continue;
      if (s->functionRoot && s->params.count(name)) continue;
      f->annexBHoisted = true;
      if (ctx_->vs) ctx_->vs->annexBFunctions.push_back(f);
    }
  } else {
    for (FunctionDeclaration* f : s->annexB) {
      auto it = s->lexical.find(f->id->name);
      if (it != s->lexical.end()) {
        // f's own block binding (alone, or with other plain function declarations of the name, whose last one
        // gives the binding its value) does not block hoisting; any other lexical declaration does.
        const Scope::Entry& e = it->second;
        bool own = e.node == f || std::find(e.duplicates.begin(), e.duplicates.end(), f) != e.duplicates.end();
        if (!own && e.kind != Lex::CatchSimple) continue;
      }
      s->parent->annexB.push_back(f);
    }
  }
  scope_ = s->parent;
  // Nothing refers to an exited block, catch, for or switch scope: reuse it (2^21 sibling blocks need one).
  if (s->kind != Scope::Kind::Root) {
    *s = Scope();
    freeScopes_.push_back(s);
  }
}

void Parser::declareVarName(const std::u16string& name, uint32_t pos) {
  for (Scope* s = scope_; s; s = s->parent) {
    auto it = s->lexical.find(name);
    if (it != s->lexical.end() && it->second.kind != Lex::CatchSimple)
      raise(pos, "Identifier '" + utf8(name) + "' has already been declared");
    s->vars.insert(name);
    if (s->kind == Scope::Kind::Root) break;
  }
  if (ctx_->vs && ctx_->varNameSet.insert(name).second) ctx_->vs->varNames.push_back(name);
}

void Parser::declareLexicalName(const std::u16string& name, Lex kind, Node* node, uint32_t pos) {
  Scope* s = scope_;
  auto it = s->lexical.find(name);
  if (it != s->lexical.end()) {
    bool annexBDuplicate = !ctx_->strict && kind == Lex::PlainFunction && it->second.kind == Lex::PlainFunction;
    if (!annexBDuplicate) raise(pos, "Identifier '" + utf8(name) + "' has already been declared");
    it->second.count++;
    it->second.duplicates.push_back(node);
    return;
  }
  if (s->vars.count(name) || (s->kind == Scope::Kind::Root && s->functions.count(name)) ||
      (s->functionRoot && s->params.count(name)))
    raise(pos, "Identifier '" + utf8(name) + "' has already been declared");
  s->lexical.emplace(name, Scope::Entry{kind, node, 1, {}});
}

void Parser::declareFunction(FunctionDeclaration* fn, uint32_t pos) {
  const std::u16string& name = fn->id->name;
  Scope* s = scope_;
  if (s->kind == Scope::Kind::Root && !s->moduleRoot) {
    if (s->lexical.count(name)) raise(pos, "Identifier '" + utf8(name) + "' has already been declared");
    s->functions.insert(name);
    ctx_->topFunctions.push_back(fn);
    if (ctx_->vs) {
      if (ctx_->varNameSet.insert(name).second) ctx_->vs->varNames.push_back(name);
      ctx_->vs->varScopedDeclarations.push_back(fn);
    }
    return;
  }
  bool plain = !fn->async && !fn->generator;
  declareLexicalName(name, plain ? Lex::PlainFunction : Lex::OtherFunction, fn, pos);
  addLexicalDeclaration(fn, {name}, false);
}

void Parser::addVarDeclarator(VariableDeclarator* d) {
  if (ctx_->vs) ctx_->vs->varScopedDeclarations.push_back(d);
}

void Parser::addLexicalDeclaration(Node* node, std::vector<std::u16string> names, bool isConstant) {
  if (!scope_->annotation) return;
  LexicalDeclaration decl;
  decl.node = node;
  decl.boundNames = std::move(names);
  decl.isConstant = isConstant;
  scope_->annotation->push_back(std::move(decl));
}

// ---------------------------------------------------------------------------------------------------
// Private names

void Parser::enterPrivateScope() {
  auto owned = std::make_unique<PrivateScope>();
  owned->parent = private_;
  private_ = owned.get();
  privatePool_.push_back(std::move(owned));
}

void Parser::exitPrivateScope() {
  PrivateScope* p = private_;
  private_ = p->parent;
  for (auto& [name, pos] : p->used) {
    if (p->declared.count(name)) continue;
    if (private_) {
      private_->used.emplace_back(name, pos);
      continue;
    }
    if (std::find(options_.privateNames.begin(), options_.privateNames.end(), name) != options_.privateNames.end())
      continue;
    raise(pos, "Private field '#" + utf8(name) + "' must be declared in an enclosing class");
  }
}

void Parser::usePrivateName(const std::u16string& name, uint32_t pos) {
  if (private_) {
    private_->used.emplace_back(name, pos);
    return;
  }
  if (std::find(options_.privateNames.begin(), options_.privateNames.end(), name) == options_.privateNames.end())
    raise(pos, "Private field '#" + utf8(name) + "' must be declared in an enclosing class");
}

PrivateIdentifier* Parser::parsePrivateIdent() {
  if (!is(T::PrivateName)) unexpected();
  PrivateIdentifier* id = startNode<PrivateIdentifier>();
  id->name = tok_.str;
  next();
  return finish(id);
}

// ---------------------------------------------------------------------------------------------------
// Identifiers

void Parser::checkUnreservedName(const std::u16string& name, K kw, bool escaped, uint32_t pos, uint32_t end) {
  (void)end;
  if (kw == K::None) return;
  if (kw == K::Yield) {
    if (yieldIsKeyword()) raise(pos, "Cannot use 'yield' as an identifier inside a generator");
    if (ctx_->strict) raise(pos, "'yield' is a reserved word in strict mode");
    return;
  }
  if (kw == K::Await) {
    if (awaitIsKeyword()) raise(pos, "Cannot use 'await' as an identifier here");
    if (!ctx_->awaitIdentPos) ctx_->awaitIdentPos = pos + 1;
    return;
  }
  if (isAlwaysReserved(kw))
    raise(pos, escaped ? "Keyword must not contain escaped characters" : "Unexpected keyword '" + utf8(name) + "'");
  if (ctx_->strict && isStrictReserved(kw)) raise(pos, "Unexpected strict mode reserved word '" + utf8(name) + "'");
}

void Parser::checkUnreserved(const Token& t) { checkUnreservedName(t.str, t.kw, t.escaped, t.start, t.end); }

Identifier* Parser::identFromToken() {
  Identifier* id = startNode<Identifier>();
  id->name = tok_.str;
  id->end = tok_.end;
  return id;
}

Identifier* Parser::parseIdent(bool liberal) {
  if (!isName()) unexpected();
  if (!liberal) checkUnreserved(tok_);
  Identifier* id = identFromToken();
  next();
  return id;
}

// ---------------------------------------------------------------------------------------------------
// Annotations

void Parser::finishVarScope(VarScope* vs, FnCtx* c) {
  vs->strict = c->strict;
  vs->hasDirectEval = vs->hasDirectEval || c->evalCount > 0;
  std::unordered_set<std::u16string> seen;
  std::vector<FunctionDeclaration*> reversed;
  for (size_t i = c->topFunctions.size(); i-- > 0;) {
    FunctionDeclaration* f = c->topFunctions[i];
    if (seen.insert(f->id->name).second) reversed.push_back(f);
  }
  vs->functionsToInitialize.assign(reversed.rbegin(), reversed.rend());
}

void Parser::finishFunctionAnnotations(Function* fn, FnCtx* c) {
  finishVarScope(fn, c);
  bool needed = fn->kind != FunctionKind::Arrow;
  if (needed && std::find(fn->parameterNames.begin(), fn->parameterNames.end(), u"arguments") != fn->parameterNames.end())
    needed = false;
  if (needed && !fn->hasParameterExpressions &&
      (c->root->functions.count(u"arguments") || c->root->lexical.count(u"arguments")))
    needed = false;
  fn->argumentsObjectNeeded = needed;
  fn->referencesArguments = c->argumentsCount > 0 || fn->hasDirectEval;
}

// ---------------------------------------------------------------------------------------------------
// Entry points

Program* Parser::parseProgram() {
  Program* program = program_;
  program->sourceType = module_ ? SourceType::Module : SourceType::Script;
  program->line = 1;
  enterFn(module_ ? FnCtx::Kind::Module : FnCtx::Kind::Script, false, false, program);
  next();
  parseTopLevel(program);
  program->start = 0;
  program->end = static_cast<uint32_t>(source_.size());
  finishVarScope(program, ctx_);
  program->hasTopLevelAwait = ctx_->usesTopLevelAwait;
  if (module_) finishModule(program);
  exitFn();
  return program;
}

ParseResult parseProgram(std::u16string source, const ParseOptions& options) {
  ParseResult result;
  try {
    Parser parser(std::move(source), options);
    result.program = parser.parseProgram();
  } catch (SyntaxErrorException& e) {
    result.program = nullptr;
    result.error = e.error;
  }
  return result;
}

ParseResult parseDynamicFunction(DynamicFunctionKind kind, std::u16string params, std::u16string body) {
  ParseResult result;
  const char16_t* prefix = u"function";
  switch (kind) {
    case DynamicFunctionKind::Normal: prefix = u"function"; break;
    case DynamicFunctionKind::Generator: prefix = u"function*"; break;
    case DynamicFunctionKind::Async: prefix = u"async function"; break;
    case DynamicFunctionKind::AsyncGenerator: prefix = u"async function*"; break;
  }
  try {
    ParseOptions options;
    {
      Parser p(params, options);
      p.checkDynamicParams(kind);
    }
    {
      Parser p(body, options);
      p.checkDynamicBody(kind);
    }
    std::u16string text = std::u16string(prefix) + u" anonymous(" + params + u"\n) {\n" + body + u"\n}";
    // The whole text must be exactly one function expression.
    std::u16string wrapped = u"(" + text + u")";
    Parser parser(wrapped, options);
    Program* program = parser.parseProgram();
    if (program->body.size() != 1 || !program->body[0]->is<ExpressionStatement>())
      throw SyntaxErrorException{ParseError{"Invalid function source", 1, 0}};
    Node* expr = program->body[0]->as<ExpressionStatement>()->expression;
    if (!expr->is<FunctionExpression>() || expr->start != 1 || expr->end != wrapped.size() - 1)
      throw SyntaxErrorException{ParseError{"Invalid function source", 1, 0}};
    result.function = expr->as<FunctionExpression>();
    result.program = program;
  } catch (SyntaxErrorException& e) {
    result.program = nullptr;
    result.function = nullptr;
    result.error = e.error;
  }
  return result;
}

}  // namespace snowman::parser
