// --dump-ast: the AST as ESTree JSON, field for field as acorn (ecmaVersion "latest") prints it.
#include <charconv>
#include <cmath>
#include <string>

#include "parser/dump.h"

#include <cstdio>

#include "parser/parser.h"

namespace snowman::parser {

using namespace ast;

namespace {

class Dumper {
 public:
  explicit Dumper(const Program* program) : source_(program->source) {}
  std::string out;

  void str(std::u16string_view s) {
    out.push_back('"');
    for (char16_t c : s) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
          if (c < 0x20 || c >= 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
            out += buf;
          } else {
            out.push_back(static_cast<char>(c));
          }
      }
    }
    out.push_back('"');
  }
  void str(const char* s) { str(std::u16string(s, s + std::char_traits<char>::length(s))); }
  void key(const char* k) {
    out += ",\"";
    out += k;
    out += "\":";
  }
  void boolean(const char* k, bool v) {
    key(k);
    out += v ? "true" : "false";
  }
  void text(const char* k, const char* v) {
    key(k);
    str(v);
  }
  void number(double d) {
    if (!std::isfinite(d)) {
      out += "null";
      return;
    }
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, d);
    out.append(buf, r.ptr);
  }
  void child(const char* k, const Node* n) {
    key(k);
    node(n);
  }
  template <class V>
  void list(const char* k, const V& items) {
    key(k);
    out.push_back('[');
    bool first = true;
    for (const Node* n : items) {
      if (!first) out.push_back(',');
      first = false;
      node(n);
    }
    out.push_back(']');
  }

  void function(const Function* f) {
    child("id", f->id);
    boolean("expression", f->expression);
    boolean("generator", f->generator);
    boolean("async", f->async);
    list("params", f->params);
    child("body", f->body);
  }
  void klass(const Class* c) {
    child("id", c->id);
    child("superClass", c->superClass);
    child("body", c->body);
  }

  void node(const Node* n);

 private:
  const std::u16string& source_;
};

const char* kindText(VariableKind k) {
  switch (k) {
    case VariableKind::Var: return "var";
    case VariableKind::Let: return "let";
    case VariableKind::Const: return "const";
    case VariableKind::Using: return "using";
    case VariableKind::AwaitUsing: return "await using";
  }
  return "var";
}

const char* kindText(MethodKind k) {
  switch (k) {
    case MethodKind::Constructor: return "constructor";
    case MethodKind::Method: return "method";
    case MethodKind::Get: return "get";
    case MethodKind::Set: return "set";
  }
  return "method";
}

const char* kindText(PropertyKind k) {
  switch (k) {
    case PropertyKind::Init: return "init";
    case PropertyKind::Get: return "get";
    case PropertyKind::Set: return "set";
  }
  return "init";
}

void Dumper::node(const Node* n) {
  if (!n) {
    out += "null";
    return;
  }
  out += "{\"type\":\"";
  out += nodeTypeName(n->type);
  out += "\",\"start\":" + std::to_string(n->start) + ",\"end\":" + std::to_string(n->end);
  switch (n->type) {
    case NodeType::Program: {
      auto* p = n->as<Program>();
      list("body", p->body);
      text("sourceType", p->sourceType == SourceType::Module ? "module" : "script");
      break;
    }
    case NodeType::Identifier:
      key("name");
      str(n->as<Identifier>()->name);
      break;
    case NodeType::PrivateIdentifier:
      key("name");
      str(n->as<PrivateIdentifier>()->name);
      break;
    case NodeType::Literal: {
      auto* l = n->as<Literal>();
      key("value");
      switch (l->kind) {
        case LiteralKind::Null: out += "null"; break;
        case LiteralKind::Boolean: out += l->boolean ? "true" : "false"; break;
        case LiteralKind::Number: number(l->number); break;
        case LiteralKind::String: str(l->string); break;
        case LiteralKind::BigInt: str(std::u16string(l->bigint.begin(), l->bigint.end())); break;
        case LiteralKind::RegExp: out += "{}"; break;
      }
      key("raw");
      str(std::u16string_view(source_).substr(l->start, l->end - l->start));
      if (l->kind == LiteralKind::RegExp) {
        key("regex");
        out += "{\"pattern\":";
        str(l->regex.pattern);
        out += ",\"flags\":";
        str(l->regex.flags);
        out += "}";
      }
      if (l->kind == LiteralKind::BigInt) {
        key("bigint");
        str(std::u16string(l->bigint.begin(), l->bigint.end()));
      }
      break;
    }
    case NodeType::ExpressionStatement: {
      auto* s = n->as<ExpressionStatement>();
      child("expression", s->expression);
      if (s->directive) {
        key("directive");
        str(*s->directive);
      }
      break;
    }
    case NodeType::BlockStatement: list("body", n->as<BlockStatement>()->body); break;
    case NodeType::EmptyStatement: break;
    case NodeType::DebuggerStatement: break;
    case NodeType::WithStatement:
      child("object", n->as<WithStatement>()->object);
      child("body", n->as<WithStatement>()->body);
      break;
    case NodeType::ReturnStatement: child("argument", n->as<ReturnStatement>()->argument); break;
    case NodeType::LabeledStatement:
      child("body", n->as<LabeledStatement>()->body);
      child("label", n->as<LabeledStatement>()->label);
      break;
    case NodeType::BreakStatement: child("label", n->as<BreakStatement>()->label); break;
    case NodeType::ContinueStatement: child("label", n->as<ContinueStatement>()->label); break;
    case NodeType::IfStatement: {
      auto* s = n->as<IfStatement>();
      child("test", s->test);
      child("consequent", s->consequent);
      child("alternate", s->alternate);
      break;
    }
    case NodeType::SwitchStatement:
      child("discriminant", n->as<SwitchStatement>()->discriminant);
      list("cases", n->as<SwitchStatement>()->cases);
      break;
    case NodeType::SwitchCase:
      list("consequent", n->as<SwitchCase>()->consequent);
      child("test", n->as<SwitchCase>()->test);
      break;
    case NodeType::ThrowStatement: child("argument", n->as<ThrowStatement>()->argument); break;
    case NodeType::TryStatement: {
      auto* s = n->as<TryStatement>();
      child("block", s->block);
      child("handler", s->handler);
      child("finalizer", s->finalizer);
      break;
    }
    case NodeType::CatchClause:
      child("param", n->as<CatchClause>()->param);
      child("body", n->as<CatchClause>()->body);
      break;
    case NodeType::WhileStatement:
      child("test", n->as<WhileStatement>()->test);
      child("body", n->as<WhileStatement>()->body);
      break;
    case NodeType::DoWhileStatement:
      child("body", n->as<DoWhileStatement>()->body);
      child("test", n->as<DoWhileStatement>()->test);
      break;
    case NodeType::ForStatement: {
      auto* s = n->as<ForStatement>();
      child("init", s->init);
      child("test", s->test);
      child("update", s->update);
      child("body", s->body);
      break;
    }
    case NodeType::ForInStatement:
      child("left", n->as<ForInStatement>()->left);
      child("right", n->as<ForInStatement>()->right);
      child("body", n->as<ForInStatement>()->body);
      break;
    case NodeType::ForOfStatement:
      boolean("await", n->as<ForOfStatement>()->await);
      child("left", n->as<ForOfStatement>()->left);
      child("right", n->as<ForOfStatement>()->right);
      child("body", n->as<ForOfStatement>()->body);
      break;
    case NodeType::FunctionDeclaration:
    case NodeType::FunctionExpression:
    case NodeType::ArrowFunctionExpression:
      function(n->as<Function>());
      break;
    case NodeType::VariableDeclaration:
      list("declarations", n->as<VariableDeclaration>()->declarations);
      text("kind", kindText(n->as<VariableDeclaration>()->kind));
      break;
    case NodeType::VariableDeclarator:
      child("id", n->as<VariableDeclarator>()->id);
      child("init", n->as<VariableDeclarator>()->init);
      break;
    case NodeType::ClassDeclaration:
    case NodeType::ClassExpression:
      klass(n->as<Class>());
      break;
    case NodeType::ThisExpression:
    case NodeType::Super:
      break;
    case NodeType::ArrayExpression: list("elements", n->as<ArrayExpression>()->elements); break;
    case NodeType::ObjectExpression: list("properties", n->as<ObjectExpression>()->properties); break;
    case NodeType::Property: {
      auto* p = n->as<Property>();
      boolean("method", p->method);
      boolean("shorthand", p->shorthand);
      boolean("computed", p->computed);
      child("key", p->key);
      child("value", p->value);
      text("kind", kindText(p->kind));
      break;
    }
    case NodeType::UnaryExpression:
      text("operator", operatorText(n->as<UnaryExpression>()->op));
      boolean("prefix", true);
      child("argument", n->as<UnaryExpression>()->argument);
      break;
    case NodeType::UpdateExpression:
      text("operator", operatorText(n->as<UpdateExpression>()->op));
      boolean("prefix", n->as<UpdateExpression>()->prefix);
      child("argument", n->as<UpdateExpression>()->argument);
      break;
    case NodeType::BinaryExpression:
      child("left", n->as<BinaryExpression>()->left);
      text("operator", operatorText(n->as<BinaryExpression>()->op));
      child("right", n->as<BinaryExpression>()->right);
      break;
    case NodeType::LogicalExpression:
      child("left", n->as<LogicalExpression>()->left);
      text("operator", operatorText(n->as<LogicalExpression>()->op));
      child("right", n->as<LogicalExpression>()->right);
      break;
    case NodeType::AssignmentExpression:
      text("operator", operatorText(n->as<AssignmentExpression>()->op));
      child("left", n->as<AssignmentExpression>()->left);
      child("right", n->as<AssignmentExpression>()->right);
      break;
    case NodeType::MemberExpression: {
      auto* m = n->as<MemberExpression>();
      child("object", m->object);
      child("property", m->property);
      boolean("computed", m->computed);
      boolean("optional", m->optional);
      break;
    }
    case NodeType::ConditionalExpression: {
      auto* c = n->as<ConditionalExpression>();
      child("test", c->test);
      child("consequent", c->consequent);
      child("alternate", c->alternate);
      break;
    }
    case NodeType::CallExpression:
      child("callee", n->as<CallExpression>()->callee);
      list("arguments", n->as<CallExpression>()->arguments);
      boolean("optional", n->as<CallExpression>()->optional);
      break;
    case NodeType::NewExpression:
      child("callee", n->as<NewExpression>()->callee);
      list("arguments", n->as<NewExpression>()->arguments);
      break;
    case NodeType::SequenceExpression: list("expressions", n->as<SequenceExpression>()->expressions); break;
    case NodeType::YieldExpression:
      boolean("delegate", n->as<YieldExpression>()->delegate);
      child("argument", n->as<YieldExpression>()->argument);
      break;
    case NodeType::AwaitExpression: child("argument", n->as<AwaitExpression>()->argument); break;
    case NodeType::TemplateLiteral:
      list("expressions", n->as<TemplateLiteral>()->expressions);
      list("quasis", n->as<TemplateLiteral>()->quasis);
      break;
    case NodeType::TaggedTemplateExpression:
      child("tag", n->as<TaggedTemplateExpression>()->tag);
      child("quasi", n->as<TaggedTemplateExpression>()->quasi);
      break;
    case NodeType::TemplateElement: {
      auto* t = n->as<TemplateElement>();
      key("value");
      out += "{\"raw\":";
      str(t->raw);
      out += ",\"cooked\":";
      if (t->cooked) str(*t->cooked);
      else out += "null";
      out += "}";
      boolean("tail", t->tail);
      break;
    }
    case NodeType::SpreadElement: child("argument", n->as<SpreadElement>()->argument); break;
    case NodeType::MetaProperty:
      child("meta", n->as<MetaProperty>()->meta);
      child("property", n->as<MetaProperty>()->property);
      break;
    case NodeType::ChainExpression: child("expression", n->as<ChainExpression>()->expression); break;
    case NodeType::ImportExpression:
      child("source", n->as<ImportExpression>()->source);
      child("options", n->as<ImportExpression>()->options);
      break;
    case NodeType::ObjectPattern: list("properties", n->as<ObjectPattern>()->properties); break;
    case NodeType::ArrayPattern: list("elements", n->as<ArrayPattern>()->elements); break;
    case NodeType::RestElement: child("argument", n->as<RestElement>()->argument); break;
    case NodeType::AssignmentPattern:
      child("left", n->as<AssignmentPattern>()->left);
      child("right", n->as<AssignmentPattern>()->right);
      break;
    case NodeType::ClassBody: list("body", n->as<ClassBody>()->body); break;
    case NodeType::MethodDefinition: {
      auto* m = n->as<MethodDefinition>();
      boolean("static", m->isStatic);
      boolean("computed", m->computed);
      child("key", m->key);
      text("kind", kindText(m->kind));
      child("value", m->value);
      break;
    }
    case NodeType::PropertyDefinition: {
      auto* p = n->as<PropertyDefinition>();
      boolean("static", p->isStatic);
      boolean("computed", p->computed);
      child("key", p->key);
      child("value", p->value);
      break;
    }
    case NodeType::StaticBlock: list("body", n->as<StaticBlock>()->body); break;
    case NodeType::ImportDeclaration:
      list("specifiers", n->as<ImportDeclaration>()->specifiers);
      child("source", n->as<ImportDeclaration>()->source);
      list("attributes", n->as<ImportDeclaration>()->attributes);
      break;
    case NodeType::ImportSpecifier:
      child("imported", n->as<ImportSpecifier>()->imported);
      child("local", n->as<ImportSpecifier>()->local);
      break;
    case NodeType::ImportDefaultSpecifier: child("local", n->as<ImportDefaultSpecifier>()->local); break;
    case NodeType::ImportNamespaceSpecifier: child("local", n->as<ImportNamespaceSpecifier>()->local); break;
    case NodeType::ImportAttribute:
      child("key", n->as<ImportAttribute>()->key);
      child("value", n->as<ImportAttribute>()->value);
      break;
    case NodeType::ExportNamedDeclaration: {
      auto* e = n->as<ExportNamedDeclaration>();
      child("declaration", e->declaration);
      list("specifiers", e->specifiers);
      child("source", e->source);
      list("attributes", e->attributes);
      break;
    }
    case NodeType::ExportSpecifier:
      child("local", n->as<ExportSpecifier>()->local);
      child("exported", n->as<ExportSpecifier>()->exported);
      break;
    case NodeType::ExportDefaultDeclaration: child("declaration", n->as<ExportDefaultDeclaration>()->declaration); break;
    case NodeType::ExportAllDeclaration:
      child("exported", n->as<ExportAllDeclaration>()->exported);
      child("source", n->as<ExportAllDeclaration>()->source);
      list("attributes", n->as<ExportAllDeclaration>()->attributes);
      break;
  }
  out.push_back('}');
}

}  // namespace

std::string dumpAstJson(const Program* program) {
  Dumper d(program);
  d.node(program);
  return std::move(d.out);
}

}  // namespace snowman::parser
