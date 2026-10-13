// Expressions, the cover grammars (arrow parameters, destructuring assignment) and patterns.
#include <cstring>

#include "parser/parser_impl.h"
#include "regexp/regexp.h"

namespace snowman::parser {

namespace {

std::string utf8(std::u16string_view s) { return utf16ToUtf8(s); }

// Binary operator precedence (acorn's numbering); -1: not a binary operator.
int binaryPrecedence(const Token& t) {
  switch (t.type) {
    case T::Nullish: return 1;
    case T::Or: return 1;
    case T::And: return 2;
    case T::Pipe: return 3;
    case T::Caret: return 4;
    case T::Amp: return 5;
    case T::Eq2: case T::NotEq: case T::Eq3: case T::NotEq2: return 6;
    case T::Lt: case T::Gt: case T::LtEq: case T::GtEq: return 7;
    case T::Shl: case T::Sar: case T::Shr: return 8;
    case T::Plus: case T::Minus: return 9;
    case T::Star: case T::Slash: case T::Percent: return 10;
    case T::Name:
      if (!t.escaped && (t.kw == K::In || t.kw == K::Instanceof)) return 7;
      return -1;
    default: return -1;
  }
}

bool isAssignOp(T t) { return t >= T::Assign && t <= T::NullishEq; }

AssignmentOperator assignOp(T t) {
  switch (t) {
    case T::PlusEq: return AssignmentOperator::Add;
    case T::MinusEq: return AssignmentOperator::Subtract;
    case T::StarEq: return AssignmentOperator::Multiply;
    case T::SlashEq: return AssignmentOperator::Divide;
    case T::PercentEq: return AssignmentOperator::Remainder;
    case T::Star2Eq: return AssignmentOperator::Exponent;
    case T::ShlEq: return AssignmentOperator::ShiftLeft;
    case T::SarEq: return AssignmentOperator::ShiftRight;
    case T::ShrEq: return AssignmentOperator::ShiftRightUnsigned;
    case T::PipeEq: return AssignmentOperator::BitOr;
    case T::CaretEq: return AssignmentOperator::BitXor;
    case T::AmpEq: return AssignmentOperator::BitAnd;
    case T::OrEq: return AssignmentOperator::Or;
    case T::AndEq: return AssignmentOperator::And;
    case T::NullishEq: return AssignmentOperator::Coalesce;
    default: return AssignmentOperator::Assign;
  }
}

bool isLocalVariableAccess(const Node* n) { return n->is<Identifier>(); }

bool isPrivateFieldAccess(const Node* n) {
  if (n->is<MemberExpression>()) return static_cast<const MemberExpression*>(n)->property->is<PrivateIdentifier>();
  if (n->is<ChainExpression>()) return isPrivateFieldAccess(static_cast<const ChainExpression*>(n)->expression);
  return false;
}

}  // namespace

bool Parser::isSimpleAssignTarget(Node* n) const { return n->is<Identifier>() || n->is<MemberExpression>(); }

bool Parser::checkPatternErrors(DestructuringErrors* r, bool isAssign) {
  if (!r) return false;
  if (r->trailingComma >= 0) raise(static_cast<uint32_t>(r->trailingComma), "Comma is not permitted after the rest element");
  int64_t parens = isAssign ? r->parenthesizedAssign : r->parenthesizedBind;
  if (parens >= 0) raise(static_cast<uint32_t>(parens), isAssign ? "Invalid assignment target" : "Invalid destructuring target");
  return false;
}

bool Parser::checkExpressionErrors(DestructuringErrors* r, bool andThrow) {
  if (!r) return false;
  if (!andThrow) return r->shorthandAssign >= 0 || r->doubleProto >= 0;
  if (r->shorthandAssign >= 0) raise(static_cast<uint32_t>(r->shorthandAssign), "Invalid shorthand property initializer");
  if (r->doubleProto >= 0) raise(static_cast<uint32_t>(r->doubleProto), "Duplicate __proto__ fields are not allowed in object literals");
  return false;
}

void Parser::checkYieldAwaitInDefaultParams() {
  if (ctx_->yieldPos && (!ctx_->awaitPos || ctx_->yieldPos < ctx_->awaitPos))
    raise(ctx_->yieldPos - 1, "Yield expression not allowed in formal parameter");
  if (ctx_->awaitPos) raise(ctx_->awaitPos - 1, "Await expression not allowed in formal parameter");
}

bool Parser::startsExpr() const {
  switch (tok_.type) {
    case T::Name:
      if (tok_.escaped) return true;
      switch (tok_.kw) {
        case K::In: case K::Instanceof: case K::Case: case K::Default: case K::Else: case K::Finally:
        case K::Catch: case K::Extends: case K::Var: case K::Const: case K::Break: case K::Continue:
        case K::Do: case K::For: case K::If: case K::Return: case K::Switch: case K::Throw: case K::Try:
        case K::While: case K::With: case K::Debugger: case K::Export: case K::Enum:
          return false;
        default:
          return true;
      }
    case T::PrivateName: case T::Num: case T::BigInt: case T::Str: case T::Template: case T::LBrace:
    case T::LParen: case T::LBracket: case T::Plus: case T::Minus: case T::Bang: case T::Tilde: case T::Inc:
    case T::Dec: case T::Slash: case T::SlashEq:
      return true;
    default:
      return false;
  }
}

Node* Parser::parseExpression(bool noIn, DestructuringErrors* refErrors) {
  Pos start = here();
  Node* expr = parseMaybeAssign(noIn, refErrors);
  if (!is(T::Comma)) return expr;
  auto* seq = startNode<SequenceExpression>(start);
  seq->expressions.push_back(expr);
  while (eat(T::Comma)) seq->expressions.push_back(parseMaybeAssign(noIn, refErrors));
  return finish(seq);
}

Node* Parser::parseMaybeAssign(bool noIn, DestructuringErrors* refErrors) {
  checkStack();
  if (isKw(K::Yield) && yieldIsKeyword()) return parseYield(noIn);
  bool ownErrors = false;
  int64_t oldParenAssign = -1, oldTrailingComma = -1, oldDoubleProto = -1;
  DestructuringErrors local;
  if (refErrors) {
    oldParenAssign = refErrors->parenthesizedAssign;
    oldTrailingComma = refErrors->trailingComma;
    oldDoubleProto = refErrors->doubleProto;
    refErrors->parenthesizedAssign = refErrors->trailingComma = -1;
  } else {
    refErrors = &local;
    ownErrors = true;
  }
  Pos start = here();
  if (is(T::LParen) || is(T::Name)) {
    potentialArrowAt_ = tok_.start;
  }
  Node* left = parseMaybeConditional(noIn, refErrors);
  if (isAssignOp(tok_.type)) {
    T opTok = tok_.type;
    auto* node = startNode<AssignmentExpression>(start);
    node->op = assignOp(opTok);
    // Annex B (web-compat): `f() = 1`, `f() += 1` in sloppy code throw at run time; never for `&&=` etc.
    bool logical = opTok == T::AndEq || opTok == T::OrEq || opTok == T::NullishEq;
    bool callTarget = !ctx_->strict && !logical && left->is<CallExpression>();
    if (opTok == T::Assign && !callTarget) left = toAssignable(left, false, refErrors);
    if (!ownErrors) refErrors->parenthesizedAssign = refErrors->trailingComma = refErrors->doubleProto = -1;
    if (refErrors->shorthandAssign >= static_cast<int64_t>(left->start)) refErrors->shorthandAssign = -1;
    if (callTarget) {
      // a run-time ReferenceError
    } else if (opTok == T::Assign) {
      checkLValPattern(left, Bind::None);
    } else {
      checkLValSimple(left, Bind::None);
    }
    node->left = left;
    next();
    node->right = parseMaybeAssign(noIn);
    if (oldDoubleProto > -1) refErrors->doubleProto = oldDoubleProto;
    return finish(node);
  }
  if (ownErrors) checkExpressionErrors(refErrors, true);
  if (oldParenAssign > -1) refErrors->parenthesizedAssign = oldParenAssign;
  if (oldTrailingComma > -1) refErrors->trailingComma = oldTrailingComma;
  return left;
}

Node* Parser::parseMaybeConditional(bool noIn, DestructuringErrors* refErrors) {
  Pos start = here();
  Node* expr = parseExprOps(noIn, refErrors);
  if (checkExpressionErrors(refErrors, false)) return expr;
  if (eat(T::Question)) {
    auto* node = startNode<ConditionalExpression>(start);
    node->test = expr;
    node->consequent = parseMaybeAssign();
    expect(T::Colon);
    node->alternate = parseMaybeAssign(noIn);
    return finish(node);
  }
  return expr;
}

Node* Parser::parseExprOps(bool noIn, DestructuringErrors* refErrors) {
  Pos start = here();
  Node* expr = parseMaybeUnary(refErrors, false, false, noIn);
  if (checkExpressionErrors(refErrors, false)) return expr;
  if (expr->start == start.start && expr->is<ArrowFunctionExpression>() && !expr->parenthesized) return expr;
  return parseExprOp(expr, start, -1, noIn);
}

Node* Parser::parseExprOp(Node* left, Pos leftStart, int minPrec, bool noIn) {
  int prec = binaryPrecedence(tok_);
  if (prec < 0 || (noIn && isKw(K::In))) return left;
  if (left->is<PrivateIdentifier>() && !isKw(K::In)) raise(left->start, "Unexpected private name");
  if (prec > minPrec) {
    T op = tok_.type;
    bool logical = op == T::Or || op == T::And;
    bool coalesce = op == T::Nullish;
    if (coalesce) prec = 2;
    Token opTok = tok_;
    next();
    Pos rightStart = here();
    Node* right = parseExprOp(parseMaybeUnary(nullptr, false, false, noIn), rightStart, op == T::Star2 ? prec - 1 : prec, noIn);
    Node* node = buildBinary(leftStart, left, right, opTok.type == T::Name ? (opTok.kw == K::In ? T::EOS : T::Arrow) : op);
    if ((logical && is(T::Nullish)) || (coalesce && (is(T::Or) || is(T::And))))
      raise(tok_.start, "Cannot mix ?? with || or && without parentheses");
    return parseExprOp(node, leftStart, minPrec, noIn);
  }
  return left;
}

// op: the operator token; `in` is passed as T::EOS and `instanceof` as T::Arrow.
Node* Parser::buildBinary(Pos start, Node* left, Node* right, T op) {
  if (right->is<PrivateIdentifier>()) raise(right->start, "Unexpected private name");
  if (left->is<PrivateIdentifier>() && op != T::EOS) raise(left->start, "Unexpected private name");
  if (op == T::Or || op == T::And || op == T::Nullish) {
    auto* node = startNode<LogicalExpression>(start);
    node->op = op == T::Or ? LogicalOperator::Or : op == T::And ? LogicalOperator::And : LogicalOperator::Coalesce;
    node->left = left;
    node->right = right;
    return finish(node);
  }
  auto* node = startNode<BinaryExpression>(start);
  BinaryOperator b = BinaryOperator::Add;
  switch (op) {
    case T::EOS: b = BinaryOperator::In; break;
    case T::Arrow: b = BinaryOperator::Instanceof; break;
    case T::Pipe: b = BinaryOperator::BitOr; break;
    case T::Caret: b = BinaryOperator::BitXor; break;
    case T::Amp: b = BinaryOperator::BitAnd; break;
    case T::Eq2: b = BinaryOperator::Equal; break;
    case T::NotEq: b = BinaryOperator::NotEqual; break;
    case T::Eq3: b = BinaryOperator::StrictEqual; break;
    case T::NotEq2: b = BinaryOperator::StrictNotEqual; break;
    case T::Lt: b = BinaryOperator::Less; break;
    case T::Gt: b = BinaryOperator::Greater; break;
    case T::LtEq: b = BinaryOperator::LessEqual; break;
    case T::GtEq: b = BinaryOperator::GreaterEqual; break;
    case T::Shl: b = BinaryOperator::ShiftLeft; break;
    case T::Sar: b = BinaryOperator::ShiftRight; break;
    case T::Shr: b = BinaryOperator::ShiftRightUnsigned; break;
    case T::Plus: b = BinaryOperator::Add; break;
    case T::Minus: b = BinaryOperator::Subtract; break;
    case T::Star: b = BinaryOperator::Multiply; break;
    case T::Slash: b = BinaryOperator::Divide; break;
    case T::Percent: b = BinaryOperator::Remainder; break;
    case T::Star2: b = BinaryOperator::Exponent; break;
    default: break;
  }
  node->op = b;
  node->left = left;
  node->right = right;
  return finish(node);
}

Node* Parser::parseMaybeUnary(DestructuringErrors* refErrors, bool sawUnary, bool incDec, bool noIn) {
  checkStack();
  Pos start = here();
  Node* expr;
  if (isKw(K::Await) && canAwait()) {
    expr = parseAwait(noIn);
    sawUnary = true;
  } else if (is(T::Bang) || is(T::Tilde) || is(T::Plus) || is(T::Minus) || is(T::Inc) || is(T::Dec) ||
             isKw(K::Typeof) || isKw(K::Void) || isKw(K::Delete)) {
    bool update = is(T::Inc) || is(T::Dec);
    T op = tok_.type;
    K kw = tok_.kw;
    next();
    Node* argument = parseMaybeUnary(nullptr, true, update, noIn);
    checkExpressionErrors(refErrors, true);
    if (update) {
      if (ctx_->strict || !argument->is<CallExpression>()) checkLValSimple(argument, Bind::None);
      auto* node = startNode<UpdateExpression>(start);
      node->op = op == T::Inc ? UpdateOperator::Increment : UpdateOperator::Decrement;
      node->prefix = true;
      node->argument = argument;
      expr = finish(node);
    } else {
      auto* node = startNode<UnaryExpression>(start);
      switch (op) {
        case T::Bang: node->op = UnaryOperator::Not; break;
        case T::Tilde: node->op = UnaryOperator::BitNot; break;
        case T::Plus: node->op = UnaryOperator::Plus; break;
        case T::Minus: node->op = UnaryOperator::Minus; break;
        default:
          node->op = kw == K::Typeof ? UnaryOperator::Typeof : kw == K::Void ? UnaryOperator::Void : UnaryOperator::Delete;
      }
      node->argument = argument;
      if (node->op == UnaryOperator::Delete) {
        if (ctx_->strict && isLocalVariableAccess(argument))
          raise(start.start, "Delete of an unqualified identifier in strict mode");
        if (isPrivateFieldAccess(argument)) raise(start.start, "Private fields can not be deleted");
      }
      sawUnary = true;
      expr = finish(node);
    }
  } else if (!sawUnary && is(T::PrivateName)) {
    if (noIn) unexpected();
    PrivateIdentifier* id = parsePrivateIdent();
    usePrivateName(id->name, id->start);
    if (!isKw(K::In)) unexpected();
    expr = id;
  } else {
    expr = parseExprSubscripts(refErrors, noIn);
    if (checkExpressionErrors(refErrors, false)) return expr;
    while ((is(T::Inc) || is(T::Dec)) && !canInsertSemicolon()) {
      auto* node = startNode<UpdateExpression>(start);
      node->op = is(T::Inc) ? UpdateOperator::Increment : UpdateOperator::Decrement;
      node->prefix = false;
      node->argument = expr;
      if (ctx_->strict || !expr->is<CallExpression>()) checkLValSimple(expr, Bind::None);
      next();
      expr = finish(node);
    }
  }
  if (!incDec && is(T::Star2)) {
    if (sawUnary) raise(tok_.start, "Unary operator used immediately before exponentiation expression");
    next();
    Node* right = parseMaybeUnary(nullptr, false, false, noIn);
    return buildBinary(start, expr, right, T::Star2);
  }
  return expr;
}

Node* Parser::parseYield(bool noIn) {
  if (!ctx_->yieldPos) ctx_->yieldPos = tok_.start + 1;
  auto* node = startNode<YieldExpression>();
  next();
  if (is(T::Semi) || canInsertSemicolon() || (!is(T::Star) && !startsExpr())) {
    node->delegate = false;
    node->argument = nullptr;
  } else {
    node->delegate = eat(T::Star);
    node->argument = parseMaybeAssign(noIn);
  }
  return finish(node);
}

Node* Parser::parseAwait(bool noIn) {
  if (!ctx_->awaitPos) ctx_->awaitPos = tok_.start + 1;
  if (ctx_->kind == FnCtx::Kind::Module) ctx_->usesTopLevelAwait = true;
  auto* node = startNode<AwaitExpression>();
  next();
  node->argument = parseMaybeUnary(nullptr, true, false, noIn);
  return finish(node);
}

Node* Parser::parseExprSubscripts(DestructuringErrors* refErrors, bool noIn) {
  Pos start = here();
  Node* expr = parseExprAtom(refErrors, noIn);
  if (expr->is<ArrowFunctionExpression>() && !expr->parenthesized) return expr;
  Node* result = parseSubscripts(expr, start, false, noIn);
  if (refErrors && result->is<MemberExpression>()) {
    if (refErrors->parenthesizedAssign >= static_cast<int64_t>(result->start)) refErrors->parenthesizedAssign = -1;
    if (refErrors->parenthesizedBind >= static_cast<int64_t>(result->start)) refErrors->parenthesizedBind = -1;
    if (refErrors->trailingComma >= static_cast<int64_t>(result->start)) refErrors->trailingComma = -1;
  }
  return result;
}

Node* Parser::parseSubscripts(Node* base, Pos start, bool noCalls, bool noIn) {
  bool maybeAsyncArrow = base->is<Identifier>() && base->as<Identifier>()->name == u"async" && prevEnd_ == base->end &&
                         !canInsertSemicolon() && base->end - base->start == 5 &&
                         potentialArrowAt_ == static_cast<int64_t>(base->start);
  bool optionalChained = false;
  for (;;) {
    bool optional = false;
    if (is(T::QuestionDot)) {
      if (noCalls) raise(tok_.start, "Invalid optional chain from new expression");
      if (base->is<Super>()) unexpected();
      optional = true;
      next();
    }
    bool computed = eat(T::LBracket);
    if (computed || (optional && !is(T::LParen) && !is(T::Template)) || eat(T::Dot)) {
      auto* node = startNode<MemberExpression>(start);
      node->object = base;
      if (computed) {
        node->property = parseExpression();
        expect(T::RBracket);
      } else if (is(T::PrivateName) && !base->is<Super>()) {
        PrivateIdentifier* id = parsePrivateIdent();
        usePrivateName(id->name, id->start);
        node->property = id;
      } else {
        node->property = parseIdent(true);
      }
      node->computed = computed;
      node->optional = optional;
      base = finish(node);
      if (optional) optionalChained = true;
      maybeAsyncArrow = false;
      continue;
    }
    if (!noCalls && is(T::LParen)) {
      next();
      DestructuringErrors errors;
      uint32_t oldYield = ctx_->yieldPos, oldAwait = ctx_->awaitPos, oldAwaitIdent = ctx_->awaitIdentPos;
      uint32_t evalBefore = ctx_->evalCount, argsBefore = ctx_->argumentsCount;
      ctx_->yieldPos = ctx_->awaitPos = ctx_->awaitIdentPos = 0;
      std::vector<Node*> args = parseExprList(T::RParen, true, false, &errors);
      if (maybeAsyncArrow && !optional && !optionalChained && is(T::Arrow) && !tok_.nlBefore) {
        checkPatternErrors(&errors, false);
        checkYieldAwaitInDefaultParams();
        if (ctx_->awaitIdentPos) raise(ctx_->awaitIdentPos - 1, "Cannot use 'await' as identifier inside an async function");
        ctx_->yieldPos = oldYield;
        ctx_->awaitPos = oldAwait;
        ctx_->awaitIdentPos = oldAwaitIdent;
        next();  // =>
        return parseArrowExpression(start, std::move(args), true, noIn, evalBefore, argsBefore);
      }
      checkExpressionErrors(&errors, true);
      if (oldYield) ctx_->yieldPos = oldYield;
      if (oldAwait) ctx_->awaitPos = oldAwait;
      if (oldAwaitIdent) ctx_->awaitIdentPos = oldAwaitIdent;
      auto* node = startNode<CallExpression>(start);
      node->callee = base;
      node->arguments = std::move(args);
      node->optional = optional;
      if (!optional && base->is<Identifier>() && base->as<Identifier>()->name == u"eval") {
        node->isDirectEvalCandidate = true;
        markDirectEval();
      }
      base = finish(node);
      if (optional) optionalChained = true;
      maybeAsyncArrow = false;
      continue;
    }
    if (is(T::Template)) {
      if (optional || optionalChained) raise(tok_.start, "Invalid tagged template on optional chain");
      auto* node = startNode<TaggedTemplateExpression>(start);
      node->tag = base;
      node->quasi = parseTemplate(true);
      base = finish(node);
      maybeAsyncArrow = false;
      continue;
    }
    if (optional) unexpected();
    break;
  }
  if (optionalChained) {
    auto* chain = startNode<ChainExpression>(start);
    chain->expression = base;
    return finish(chain);
  }
  return base;
}

Literal* Parser::parseLiteralToken() {
  auto* lit = startNode<Literal>();
  switch (tok_.type) {
    case T::Num:
      if (tok_.legacyOctal && ctx_->strict) raise(tok_.start, "Octal literals are not allowed in strict mode");
      lit->kind = LiteralKind::Number;
      lit->number = tok_.num;
      break;
    case T::BigInt:
      lit->kind = LiteralKind::BigInt;
      lit->bigint = tok_.bigint;
      break;
    case T::Str:
      if (tok_.octalEscape >= 0 && ctx_->strict)
        raise(static_cast<uint32_t>(tok_.octalEscape), "Octal escape sequences are not allowed in strict mode");
      lit->kind = LiteralKind::String;
      lit->string = tok_.str;
      break;
    default:
      unexpected();
  }
  lit->end = tok_.end;
  next();
  return lit;
}

TemplateLiteral* Parser::parseTemplate(bool tagged) {
  auto* node = startNode<TemplateLiteral>();
  for (;;) {
    if (!is(T::Template)) unexpected();
    auto* el = make<TemplateElement>();
    bool tail = tok_.templateTail;
    el->start = tok_.start + 1;
    el->end = tok_.end - (tail ? 1 : 2);
    el->line = tok_.line;
    el->column = tok_.col + 1;
    el->raw = tok_.raw;
    if (tok_.cookedInvalid) {
      if (!tagged) raise(tok_.invalidEscapePos, "Invalid escape sequence in template");
    } else {
      el->cooked = tok_.str;
    }
    el->tail = tail;
    node->quasis.push_back(el);
    next();
    if (tail) break;
    node->expressions.push_back(parseExpression());
    if (!is(T::RBrace)) unexpected();
    lexer_.rescanTemplate(tok_);
  }
  return finish(node);
}

Node* Parser::parseExprAtom(DestructuringErrors* refErrors, bool noIn, bool forNew) {
  Pos start = here();
  bool canBeArrow = potentialArrowAt_ == static_cast<int64_t>(tok_.start);
  switch (tok_.type) {
    case T::Name: {
      if (!tok_.escaped) {
        switch (tok_.kw) {
          case K::This: {
            auto* node = startNode<ThisExpression>();
            next();
            return finish(node);
          }
          case K::Super: {
            if (!ctx_->allowSuperProperty && !ctx_->allowSuperCall) raise(tok_.start, "'super' keyword unexpected here");
            auto* node = startNode<Super>();
            next();
            if (is(T::LParen) && (!ctx_->allowSuperCall || forNew)) raise(start.start, "'super' keyword unexpected here");
            if (!is(T::Dot) && !is(T::LBracket) && !is(T::LParen)) raise(start.start, "'super' keyword unexpected here");
            if (!is(T::LParen) && !ctx_->allowSuperProperty) raise(start.start, "'super' keyword unexpected here");
            return finish(node);
          }
          case K::Null:
          case K::True:
          case K::False: {
            auto* lit = startNode<Literal>();
            lit->kind = tok_.kw == K::Null ? LiteralKind::Null : LiteralKind::Boolean;
            lit->boolean = tok_.kw == K::True;
            next();
            return finish(lit);
          }
          case K::Function: {
            auto* fn = startNode<FunctionExpression>();
            next();
            parseFunction(fn, false, false, false, false);
            return fn;
          }
          case K::Class:
            return parseClass(startNode<ClassExpression>(), false, true);
          case K::New:
            return parseNew();
          case K::Import:
            return parseImportExpr(forNew);
          default:
            break;
        }
      }
      Token idTok = tok_;
      if (isAsyncFunction()) {
        auto* fn = startNode<FunctionExpression>();
        next();
        next();
        parseFunction(fn, false, false, true, false);
        return fn;
      }
      Identifier* id = parseIdent(false);
      if (canBeArrow && !canInsertSemicolon()) {
        if (is(T::Arrow)) {
          next();
          return parseArrowExpression(start, {id}, false, noIn, ctx_->evalCount, ctx_->argumentsCount);
        }
        if (idTok.kw == K::Async && !idTok.escaped && isName() && !(potentialArrowInForAwait_ && isKw(K::Of))) {
          if (tok_.kw == K::Await) raise(tok_.start, "Cannot use 'await' as identifier inside an async function");
          Identifier* param = parseIdent(false);
          if (canInsertSemicolon() || !is(T::Arrow)) unexpected();
          next();
          return parseArrowExpression(start, {param}, true, noIn, ctx_->evalCount, ctx_->argumentsCount);
        }
      }
      if (id->name == u"arguments") markArguments();
      return id;
    }
    case T::Num:
    case T::BigInt:
    case T::Str:
      return parseLiteralToken();
    case T::Slash:
    case T::SlashEq: {
      lexer_.rescanRegExp(tok_);
      auto* lit = startNode<Literal>();
      lit->kind = LiteralKind::RegExp;
      lit->regex.pattern = tok_.str;
      lit->regex.flags = tok_.raw;
      if (auto error = regexp::validate(lit->regex.pattern, lit->regex.flags))
        raise(tok_.start, "Invalid regular expression: /" + utf8(lit->regex.pattern) + "/" + utf8(lit->regex.flags) + ": " + *error);
      lit->end = tok_.end;
      next();
      return lit;
    }
    case T::LParen: {
      uint32_t parenStart = tok_.start;
      Node* expr = parseParenAndDistinguish(canBeArrow, noIn);
      if (refErrors) {
        if (refErrors->parenthesizedAssign < 0 && !isSimpleAssignTarget(expr)) refErrors->parenthesizedAssign = parenStart;
        if (refErrors->parenthesizedBind < 0) refErrors->parenthesizedBind = parenStart;
      }
      return expr;
    }
    case T::LBracket: {
      auto* node = startNode<ArrayExpression>();
      next();
      node->elements = parseExprList(T::RBracket, true, true, refErrors);
      return finish(node);
    }
    case T::LBrace:
      return parseObj(false, refErrors);
    case T::Template:
      return parseTemplate(false);
    default:
      unexpected();
  }
}

Node* Parser::parseParenAndDistinguish(bool canBeArrow, bool noIn) {
  Pos start = here();
  next();  // (
  uint32_t oldYield = ctx_->yieldPos, oldAwait = ctx_->awaitPos;
  uint32_t evalBefore = ctx_->evalCount, argsBefore = ctx_->argumentsCount;
  ctx_->yieldPos = ctx_->awaitPos = 0;
  Pos innerStart = here();
  std::vector<Node*> list;
  bool first = true, lastIsComma = false;
  DestructuringErrors errors;
  int64_t spreadStart = -1;
  while (!is(T::RParen)) {
    if (first) {
      first = false;
    } else {
      expect(T::Comma);
      if (is(T::RParen)) {
        lastIsComma = true;
        break;
      }
    }
    if (is(T::Ellipsis)) {
      spreadStart = tok_.start;
      list.push_back(parseRestBinding());
      if (is(T::Comma)) raise(tok_.start, "Comma is not permitted after the rest element");
      break;
    }
    list.push_back(parseMaybeAssign(false, &errors));
  }
  uint32_t innerEnd = prevEnd_;
  Token close = tok_;
  expect(T::RParen);
  if (canBeArrow && is(T::Arrow) && !tok_.nlBefore) {
    checkPatternErrors(&errors, false);
    checkYieldAwaitInDefaultParams();
    ctx_->yieldPos = oldYield;
    ctx_->awaitPos = oldAwait;
    next();  // =>
    return parseArrowExpression(start, std::move(list), false, noIn, evalBefore, argsBefore);
  }
  if (list.empty() || lastIsComma) unexpectedAt(close);
  if (spreadStart >= 0) raise(static_cast<uint32_t>(spreadStart), "Unexpected token '...'");
  checkExpressionErrors(&errors, true);
  if (oldYield) ctx_->yieldPos = oldYield;
  if (oldAwait) ctx_->awaitPos = oldAwait;
  Node* value;
  if (list.size() > 1) {
    auto* seq = startNode<SequenceExpression>(innerStart);
    seq->expressions = std::move(list);
    seq->end = innerEnd;
    value = seq;
  } else {
    value = list[0];
  }
  value->parenthesized = true;
  return value;
}

Node* Parser::parseArrowExpression(Pos start, std::vector<Node*> params, bool isAsync, bool noIn, uint32_t evalBefore,
                                   uint32_t argsBefore) {
  auto* fn = startNode<ArrowFunctionExpression>(start);
  fn->async = isAsync;
  fn->kind = FunctionKind::Arrow;
  uint32_t paramEvals = ctx_->evalCount - evalBefore;
  uint32_t paramArgs = ctx_->argumentsCount - argsBefore;
  uint32_t oldYield = ctx_->yieldPos, oldAwait = ctx_->awaitPos, oldAwaitIdent = ctx_->awaitIdentPos;
  FnCtx* c = enterFn(FnCtx::Kind::Arrow, isAsync, false, fn);
  c->evalCount += paramEvals;
  c->argumentsCount += paramArgs;
  toAssignableList(params, true);
  fn->params = std::move(params);
  computeParamInfo(fn);
  std::vector<std::u16string> names;
  std::vector<std::u16string>* saved = collect_;
  collect_ = &names;
  for (Node* p : fn->params) checkLValInnerPattern(p, Bind::Param);
  collect_ = saved;
  for (auto& name : names) c->root->params.insert(name);
  fn->parameterNames = std::move(names);
  parseFunctionBody(fn, true, false, noIn);
  finishFunctionAnnotations(fn, c);
  exitFn();
  ctx_->yieldPos = oldYield;
  ctx_->awaitPos = oldAwait;
  ctx_->awaitIdentPos = oldAwaitIdent;
  finish(fn);
  fn->sourceText = sliceOf(source_, fn);
  return fn;
}

Node* Parser::parseNew() {
  Pos start = here();
  next();  // new
  if (is(T::Dot)) {
    auto* node = startNode<MetaProperty>(start);
    auto* meta = startNode<Identifier>(start);
    meta->name = u"new";
    meta->end = start.start + 3;
    node->meta = meta;
    next();
    Token t = tok_;
    node->property = parseIdent(true);
    if (node->property->name != u"target") raise(t.start, "The only valid meta property for new is 'new.target'");
    if (t.escaped) raise(t.start, "'new.target' must not contain escaped characters");
    if (!ctx_->allowNewTarget) raise(start.start, "new.target expression is not allowed here");
    return finish(node);
  }
  Pos calleeStart = here();
  auto* node = startNode<NewExpression>(start);
  node->callee = parseSubscripts(parseExprAtom(nullptr, false, true), calleeStart, true, false);
  if (eat(T::LParen)) node->arguments = parseExprList(T::RParen, true, false, nullptr);
  return finish(node);
}

Node* Parser::parseImportExpr(bool forNew) {
  Pos start = here();
  next();  // import
  if (is(T::LParen) && !forNew) {
    auto* node = startNode<ImportExpression>(start);
    next();
    node->source = parseMaybeAssign();
    if (eat(T::Comma)) {
      if (!is(T::RParen)) {
        node->options = parseMaybeAssign();
        eat(T::Comma);
      }
    }
    expect(T::RParen);
    return finish(node);
  }
  if (is(T::Dot)) {
    auto* node = startNode<MetaProperty>(start);
    auto* meta = startNode<Identifier>(start);
    meta->name = u"import";
    meta->end = start.start + 6;
    node->meta = meta;
    next();
    Token t = tok_;
    node->property = parseIdent(true);
    if (node->property->name != u"meta") raise(t.start, "The only valid meta property for import is 'import.meta'");
    if (t.escaped) raise(t.start, "'import.meta' must not contain escaped characters");
    if (!module_) raise(start.start, "Cannot use 'import.meta' outside a module");
    return finish(node);
  }
  unexpected();
}

std::vector<Node*> Parser::parseExprList(T close, bool allowTrailingComma, bool allowEmpty, DestructuringErrors* refErrors) {
  std::vector<Node*> elts;
  bool first = true;
  while (!eat(close)) {
    if (!first) {
      expect(T::Comma);
      if (allowTrailingComma && is(close)) {
        next();
        break;
      }
    } else {
      first = false;
    }
    Node* elt;
    if (allowEmpty && is(T::Comma)) {
      elt = nullptr;
    } else if (is(T::Ellipsis)) {
      elt = parseSpread(refErrors);
      if (refErrors && is(T::Comma) && refErrors->trailingComma < 0) refErrors->trailingComma = tok_.start;
    } else {
      elt = parseMaybeAssign(false, refErrors);
    }
    elts.push_back(elt);
  }
  return elts;
}

Node* Parser::parseSpread(DestructuringErrors* refErrors) {
  auto* node = startNode<SpreadElement>();
  next();
  node->argument = parseMaybeAssign(false, refErrors);
  return finish(node);
}

Node* Parser::parsePropertyName(bool& computed) {
  if (eat(T::LBracket)) {
    computed = true;
    Node* key = parseMaybeAssign();
    expect(T::RBracket);
    return key;
  }
  computed = false;
  if (is(T::Num) || is(T::Str) || is(T::BigInt)) return parseLiteralToken();
  if (!isName()) unexpected();
  return parseIdent(true);
}

Node* Parser::parsePropertyName(Property* prop) {
  bool computed = false;
  prop->key = parsePropertyName(computed);
  prop->computed = computed;
  return prop->key;
}

// ---------------------------------------------------------------------------------------------------
// Object literals and patterns

Node* Parser::parseObj(bool isPattern, DestructuringErrors* refErrors) {
  Pos start = here();
  next();  // {
  std::vector<Node*> props;
  bool first = true, sawProto = false;
  while (!eat(T::RBrace)) {
    if (!first) {
      expect(T::Comma);
      if (eat(T::RBrace)) break;
    } else {
      first = false;
    }
    Node* prop = parseProperty(isPattern, refErrors);
    if (!isPattern && prop->is<Property>()) {
      auto* p = prop->as<Property>();
      if (!p->computed && !p->method && !p->shorthand && p->kind == PropertyKind::Init) {
        bool isProto = (p->key->is<Identifier>() && p->key->as<Identifier>()->name == u"__proto__") ||
                       (p->key->is<Literal>() && p->key->as<Literal>()->kind == LiteralKind::String &&
                        p->key->as<Literal>()->string == u"__proto__");
        if (isProto) {
          if (sawProto) {
            if (refErrors) {
              if (refErrors->doubleProto < 0) refErrors->doubleProto = p->key->start;
            } else {
              raise(p->key->start, "Duplicate __proto__ fields are not allowed in object literals");
            }
          }
          sawProto = true;
        }
      }
    }
    props.push_back(prop);
  }
  if (isPattern) {
    auto* node = startNode<ObjectPattern>(start);
    node->properties = std::move(props);
    return finish(node);
  }
  auto* node = startNode<ObjectExpression>(start);
  node->properties = std::move(props);
  return finish(node);
}

Node* Parser::parseProperty(bool isPattern, DestructuringErrors* refErrors) {
  Pos start = here();
  if (is(T::Ellipsis)) {
    if (isPattern) {
      auto* rest = startNode<RestElement>();
      next();
      rest->argument = parseIdent(false);
      if (is(T::Comma)) raise(tok_.start, "Comma is not permitted after the rest element");
      return finish(rest);
    }
    auto* spread = startNode<SpreadElement>();
    next();
    spread->argument = parseMaybeAssign(false, refErrors);
    if (is(T::Comma) && refErrors && refErrors->trailingComma < 0) refErrors->trailingComma = tok_.start;
    return finish(spread);
  }
  auto* prop = startNode<Property>(start);
  bool isGenerator = false, isAsync = false;
  if (!isPattern) isGenerator = eat(T::Star);
  bool containsEsc = tok_.escaped;
  Token keyTok = tok_;
  parsePropertyName(prop);
  auto isIdentKey = [&](const char16_t* name) {
    return !prop->computed && prop->key->is<Identifier>() && prop->key->as<Identifier>()->name == name;
  };
  if (!isPattern && !containsEsc && !isGenerator && isIdentKey(u"async") &&
      (is(T::Name) || is(T::Num) || is(T::Str) || is(T::BigInt) || is(T::LBracket) || is(T::Star)) && !tok_.nlBefore) {
    isAsync = true;
    isGenerator = eat(T::Star);
    keyTok = tok_;
    parsePropertyName(prop);
  }
  if ((isGenerator || isAsync) && is(T::Colon)) unexpected();
  if (eat(T::Colon)) {
    prop->value = isPattern ? parseMaybeDefault(here(), nullptr) : parseMaybeAssign(false, refErrors);
    prop->kind = PropertyKind::Init;
  } else if (is(T::LParen)) {
    if (isPattern) unexpected();
    prop->kind = PropertyKind::Init;
    prop->method = true;
    prop->value = parseMethod(start, isGenerator, isAsync, FunctionKind::Method, false);
  } else if (!isPattern && !containsEsc && (isIdentKey(u"get") || isIdentKey(u"set")) && !is(T::Comma) &&
             !is(T::RBrace) && !is(T::Assign)) {
    if (isGenerator || isAsync) unexpected();
    bool getter = isIdentKey(u"get");
    parsePropertyName(prop);
    auto* fn = parseMethod(start, false, false, getter ? FunctionKind::Getter : FunctionKind::Setter, false);
    prop->value = fn;
    prop->kind = getter ? PropertyKind::Get : PropertyKind::Set;
    if (getter && !fn->params.empty()) raise(fn->start, "Getter must not have any formal parameters");
    if (!getter && fn->params.size() != 1) raise(fn->start, "Setter must have exactly one formal parameter");
    if (!getter && fn->params[0]->is<RestElement>()) raise(fn->start, "Setter function argument must not be a rest parameter");
  } else if (!prop->computed && prop->key->is<Identifier>()) {
    if (isGenerator || isAsync) unexpected();
    checkUnreserved(keyTok);
    auto* value = startAt<Identifier>(prop->key);
    value->name = prop->key->as<Identifier>()->name;
    value->end = prop->key->end;
    if (isPattern) {
      prop->value = parseMaybeDefault(start, value);
    } else if (is(T::Assign) && refErrors) {
      if (refErrors->shorthandAssign < 0) refErrors->shorthandAssign = tok_.start;
      prop->value = parseMaybeDefault(start, value);
    } else {
      if (value->name == u"arguments") markArguments();
      prop->value = value;
    }
    prop->kind = PropertyKind::Init;
    prop->shorthand = true;
  } else {
    unexpected();
  }
  return finish(prop);
}

Node* Parser::parseBindingAtom() {
  checkStack();
  if (is(T::LBracket)) {
    auto* node = startNode<ArrayPattern>();
    next();
    node->elements = parseBindingList(T::RBracket, true, true);
    return finish(node);
  }
  if (is(T::LBrace)) return parseObj(true, nullptr);
  return parseIdent(false);
}

std::vector<Node*> Parser::parseBindingList(T close, bool allowEmpty, bool allowTrailingComma) {
  std::vector<Node*> elts;
  bool first = true;
  while (!eat(close)) {
    if (first) {
      first = false;
    } else {
      expect(T::Comma);
    }
    if (allowEmpty && is(T::Comma)) {
      elts.push_back(nullptr);
    } else if (allowTrailingComma && is(close)) {
      next();
      break;
    } else if (is(T::Ellipsis)) {
      elts.push_back(parseRestBinding());
      if (is(T::Comma)) raise(tok_.start, "Comma is not permitted after the rest element");
      expect(close);
      break;
    } else {
      elts.push_back(parseMaybeDefault(here(), nullptr));
    }
  }
  return elts;
}

Node* Parser::parseRestBinding() {
  auto* node = startNode<RestElement>();
  next();
  node->argument = parseBindingAtom();
  return finish(node);
}

Node* Parser::parseMaybeDefault(Pos start, Node* left) {
  if (!left) left = parseBindingAtom();
  if (!is(T::Assign)) return left;
  next();
  auto* node = startNode<AssignmentPattern>(start);
  node->left = left;
  node->right = parseMaybeAssign();
  return finish(node);
}

// ---------------------------------------------------------------------------------------------------
// Cover grammar conversions and binding checks

namespace {

template <class To>
To* convertNode(const Node* from) {
  To* to = make<To>();
  to->start = from->start;
  to->end = from->end;
  to->line = from->line;
  to->column = from->column;
  to->parenthesized = from->parenthesized;
  return to;
}

}  // namespace

Node* Parser::toAssignable(Node* node, bool isBinding, DestructuringErrors* refErrors) {
  switch (node->type) {
    case NodeType::Identifier:
    case NodeType::ObjectPattern:
    case NodeType::ArrayPattern:
    case NodeType::AssignmentPattern:
    case NodeType::RestElement:
      if (refErrors) checkPatternErrors(refErrors, true);
      return node;
    case NodeType::ObjectExpression: {
      if (node->parenthesized) raise(node->start, "Invalid destructuring assignment target");
      if (refErrors) checkPatternErrors(refErrors, true);
      auto* obj = node->as<ObjectExpression>();
      auto* pattern = convertNode<ObjectPattern>(node);
      for (Node* prop : obj->properties) {
        Node* converted = toAssignable(prop, isBinding, nullptr);
        if (converted->is<RestElement>()) {
          Node* arg = converted->as<RestElement>()->argument;
          if (arg->is<ArrayPattern>() || arg->is<ObjectPattern>()) raise(arg->start, "Invalid rest element");
        }
        pattern->properties.push_back(converted);
      }
      return pattern;
    }
    case NodeType::Property: {
      auto* prop = node->as<Property>();
      if (prop->kind != PropertyKind::Init || prop->method) raise(prop->key->start, "Invalid destructuring assignment target");
      prop->value = toAssignable(prop->value, isBinding, nullptr);
      return prop;
    }
    case NodeType::ArrayExpression: {
      if (node->parenthesized) raise(node->start, "Invalid destructuring assignment target");
      if (refErrors) checkPatternErrors(refErrors, true);
      auto* arr = node->as<ArrayExpression>();
      auto* pattern = convertNode<ArrayPattern>(node);
      pattern->elements = arr->elements;
      toAssignableList(pattern->elements, isBinding);
      return pattern;
    }
    case NodeType::SpreadElement: {
      auto* rest = convertNode<RestElement>(node);
      rest->argument = toAssignable(node->as<SpreadElement>()->argument, isBinding, nullptr);
      if (rest->argument->is<AssignmentPattern>()) raise(rest->argument->start, "Rest elements cannot have a default value");
      return rest;
    }
    case NodeType::AssignmentExpression: {
      auto* assign = node->as<AssignmentExpression>();
      if (assign->op != AssignmentOperator::Assign) raise(assign->left->end, "Only '=' operator can be used for specifying default value.");
      if (node->parenthesized) raise(node->start, "Invalid destructuring assignment target");
      auto* pattern = convertNode<AssignmentPattern>(node);
      pattern->left = toAssignable(assign->left, isBinding, nullptr);
      pattern->right = assign->right;
      return pattern;
    }
    case NodeType::ChainExpression:
      raise(node->start, "Optional chaining cannot appear in left-hand side");
    case NodeType::MemberExpression:
      if (!isBinding) return node;
      [[fallthrough]];
    default:
      raise(node->start, "Invalid left-hand side in assignment");
  }
}

void Parser::toAssignableList(std::vector<Node*>& list, bool isBinding) {
  for (auto& elt : list)
    if (elt) elt = toAssignable(elt, isBinding, nullptr);
}

void Parser::bindIdentifier(Identifier* id, Bind bind) {
  const std::u16string& name = id->name;
  switch (bind) {
    case Bind::None:
    case Bind::Outside:
    case Bind::Param:
      break;
    case Bind::Var:
      declareVarName(name, id->start);
      break;
    case Bind::Let:
    case Bind::Const:
    case Bind::Using:
    case Bind::Class: {
      if (name == u"let") raise(id->start, "let is disallowed as a lexically bound name");
      Lex kind = bind == Bind::Let ? Lex::Let : bind == Bind::Const ? Lex::Const : bind == Bind::Using ? Lex::Using : Lex::Class;
      declareLexicalName(name, kind, nullptr, id->start);
      break;
    }
    case Bind::CatchSimple:
      declareLexicalName(name, Lex::CatchSimple, nullptr, id->start);
      break;
    case Bind::CatchPattern:
      declareLexicalName(name, Lex::CatchPattern, nullptr, id->start);
      break;
    case Bind::Import:
      declareLexicalName(name, Lex::Import, nullptr, id->start);
      break;
  }
  if (collect_) collect_->push_back(name);
}

void Parser::checkLValSimple(Node* expr, Bind bind) {
  switch (expr->type) {
    case NodeType::Identifier: {
      auto* id = expr->as<Identifier>();
      if (ctx_->strict && (id->name == u"eval" || id->name == u"arguments"))
        raise(id->start, bind == Bind::None ? "Unexpected eval or arguments in strict mode" : "Unexpected eval or arguments in strict mode");
      if (bind != Bind::None && expr->parenthesized) raise(expr->start, "Invalid destructuring target");
      bindIdentifier(id, bind);
      return;
    }
    case NodeType::ChainExpression:
      raise(expr->start, "Optional chaining cannot appear in left-hand side");
    case NodeType::MemberExpression:
      if (bind != Bind::None) raise(expr->start, "Invalid destructuring target");
      return;
    default:
      raise(expr->start, bind == Bind::None ? "Invalid left-hand side in assignment" : "Invalid destructuring target");
  }
}

void Parser::checkLValPattern(Node* expr, Bind bind) {
  switch (expr->type) {
    case NodeType::ObjectPattern:
      if (expr->parenthesized) raise(expr->start, "Invalid destructuring target");
      for (Node* prop : expr->as<ObjectPattern>()->properties) checkLValInnerPattern(prop, bind);
      return;
    case NodeType::ArrayPattern:
      if (expr->parenthesized) raise(expr->start, "Invalid destructuring target");
      for (Node* elem : expr->as<ArrayPattern>()->elements)
        if (elem) checkLValInnerPattern(elem, bind);
      return;
    default:
      checkLValSimple(expr, bind);
  }
}

void Parser::checkLValInnerPattern(Node* expr, Bind bind) {
  switch (expr->type) {
    case NodeType::Property:
      checkLValInnerPattern(expr->as<Property>()->value, bind);
      return;
    case NodeType::AssignmentPattern:
      checkLValPattern(expr->as<AssignmentPattern>()->left, bind);
      return;
    case NodeType::RestElement:
      checkLValPattern(expr->as<RestElement>()->argument, bind);
      return;
    default:
      checkLValPattern(expr, bind);
  }
}

// ---------------------------------------------------------------------------------------------------
// CreateDynamicFunction: the parameters and the body on their own.

void Parser::checkDynamicParams(DynamicFunctionKind kind) {
  bool async = kind == DynamicFunctionKind::Async || kind == DynamicFunctionKind::AsyncGenerator;
  bool generator = kind == DynamicFunctionKind::Generator || kind == DynamicFunctionKind::AsyncGenerator;
  enterFn(FnCtx::Kind::Script, false, false, nullptr);
  auto* fn = make<FunctionExpression>();
  enterFn(FnCtx::Kind::Function, async, generator, fn);
  lexer_.next(tok_);
  fn->params = parseBindingList(T::EOS, false, true);
  checkYieldAwaitInDefaultParams();
}

void Parser::checkDynamicBody(DynamicFunctionKind kind) {
  bool async = kind == DynamicFunctionKind::Async || kind == DynamicFunctionKind::AsyncGenerator;
  bool generator = kind == DynamicFunctionKind::Generator || kind == DynamicFunctionKind::AsyncGenerator;
  enterFn(FnCtx::Kind::Script, false, false, nullptr);
  auto* fn = make<FunctionExpression>();
  enterFn(FnCtx::Kind::Function, async, generator, fn);
  lexer_.next(tok_);
  std::vector<Statement*> body;
  parseDirectives(body, nullptr, nullptr);
  while (!is(T::EOS)) body.push_back(parseStatement(nullptr, false));
}

}  // namespace snowman::parser
