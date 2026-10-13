// Static analysis the evaluator uses to skip unobservable work: whether code can capture the running
// environment (a closure or a direct eval), so a loop may reuse one environment per iteration.
#include "interp/interpreter.h"

namespace snowman::interp {

using namespace ast;

namespace {

bool anyCaptures(const std::vector<Node*>& nodes) {
  for (const Node* node : nodes) {
    if (mayCaptureEnvironment(node)) return true;
  }
  return false;
}

}  // namespace

bool mayCaptureEnvironment(const Node* node) {
  if (!node) return false;
  switch (node->type) {
    case NodeType::Identifier:
    case NodeType::PrivateIdentifier:
    case NodeType::Literal:
    case NodeType::EmptyStatement:
    case NodeType::DebuggerStatement:
    case NodeType::BreakStatement:
    case NodeType::ContinueStatement:
    case NodeType::ThisExpression:
    case NodeType::Super:
    case NodeType::MetaProperty:
      return false;
    case NodeType::ExpressionStatement:
      return mayCaptureEnvironment(node->as<ExpressionStatement>()->expression);
    case NodeType::BlockStatement:
      return anyCaptures(node->as<BlockStatement>()->body);
    case NodeType::WithStatement: {
      auto* n = node->as<WithStatement>();
      return mayCaptureEnvironment(n->object) || mayCaptureEnvironment(n->body);
    }
    case NodeType::ReturnStatement:
      return mayCaptureEnvironment(node->as<ReturnStatement>()->argument);
    case NodeType::LabeledStatement:
      return mayCaptureEnvironment(node->as<LabeledStatement>()->body);
    case NodeType::IfStatement: {
      auto* n = node->as<IfStatement>();
      return mayCaptureEnvironment(n->test) || mayCaptureEnvironment(n->consequent) ||
             mayCaptureEnvironment(n->alternate);
    }
    case NodeType::SwitchStatement: {
      auto* n = node->as<SwitchStatement>();
      if (mayCaptureEnvironment(n->discriminant)) return true;
      for (const SwitchCase* c : n->cases) {
        if (mayCaptureEnvironment(c->test) || anyCaptures(c->consequent)) return true;
      }
      return false;
    }
    case NodeType::ThrowStatement:
      return mayCaptureEnvironment(node->as<ThrowStatement>()->argument);
    case NodeType::TryStatement: {
      auto* n = node->as<TryStatement>();
      if (mayCaptureEnvironment(n->block) || mayCaptureEnvironment(n->finalizer)) return true;
      return n->handler && (mayCaptureEnvironment(n->handler->param) || mayCaptureEnvironment(n->handler->body));
    }
    case NodeType::WhileStatement: {
      auto* n = node->as<WhileStatement>();
      return mayCaptureEnvironment(n->test) || mayCaptureEnvironment(n->body);
    }
    case NodeType::DoWhileStatement: {
      auto* n = node->as<DoWhileStatement>();
      return mayCaptureEnvironment(n->test) || mayCaptureEnvironment(n->body);
    }
    case NodeType::ForStatement: {
      auto* n = node->as<ForStatement>();
      return mayCaptureEnvironment(n->init) || mayCaptureEnvironment(n->test) || mayCaptureEnvironment(n->update) ||
             mayCaptureEnvironment(n->body);
    }
    case NodeType::ForInStatement: {
      auto* n = node->as<ForInStatement>();
      return mayCaptureEnvironment(n->left) || mayCaptureEnvironment(n->right) || mayCaptureEnvironment(n->body);
    }
    case NodeType::ForOfStatement: {
      auto* n = node->as<ForOfStatement>();
      return mayCaptureEnvironment(n->left) || mayCaptureEnvironment(n->right) || mayCaptureEnvironment(n->body);
    }
    case NodeType::VariableDeclaration:
      for (const VariableDeclarator* d : node->as<VariableDeclaration>()->declarations) {
        if (mayCaptureEnvironment(d->id) || mayCaptureEnvironment(d->init)) return true;
      }
      return false;
    case NodeType::ArrayExpression:
      return anyCaptures(node->as<ArrayExpression>()->elements);
    case NodeType::ObjectExpression:
      return anyCaptures(node->as<ObjectExpression>()->properties);
    case NodeType::Property: {
      auto* n = node->as<Property>();
      return mayCaptureEnvironment(n->key) || mayCaptureEnvironment(n->value);
    }
    case NodeType::UnaryExpression:
      return mayCaptureEnvironment(node->as<UnaryExpression>()->argument);
    case NodeType::UpdateExpression:
      return mayCaptureEnvironment(node->as<UpdateExpression>()->argument);
    case NodeType::BinaryExpression: {
      auto* n = node->as<BinaryExpression>();
      return mayCaptureEnvironment(n->left) || mayCaptureEnvironment(n->right);
    }
    case NodeType::LogicalExpression: {
      auto* n = node->as<LogicalExpression>();
      return mayCaptureEnvironment(n->left) || mayCaptureEnvironment(n->right);
    }
    case NodeType::AssignmentExpression: {
      auto* n = node->as<AssignmentExpression>();
      return mayCaptureEnvironment(n->left) || mayCaptureEnvironment(n->right);
    }
    case NodeType::MemberExpression: {
      auto* n = node->as<MemberExpression>();
      return mayCaptureEnvironment(n->object) || (n->computed && mayCaptureEnvironment(n->property));
    }
    case NodeType::ConditionalExpression: {
      auto* n = node->as<ConditionalExpression>();
      return mayCaptureEnvironment(n->test) || mayCaptureEnvironment(n->consequent) ||
             mayCaptureEnvironment(n->alternate);
    }
    case NodeType::CallExpression: {
      auto* n = node->as<CallExpression>();
      if (n->isDirectEvalCandidate) return true;
      return mayCaptureEnvironment(n->callee) || anyCaptures(n->arguments);
    }
    case NodeType::NewExpression: {
      auto* n = node->as<NewExpression>();
      return mayCaptureEnvironment(n->callee) || anyCaptures(n->arguments);
    }
    case NodeType::SequenceExpression:
      return anyCaptures(node->as<SequenceExpression>()->expressions);
    case NodeType::YieldExpression:
      return mayCaptureEnvironment(node->as<YieldExpression>()->argument);
    case NodeType::AwaitExpression:
      return mayCaptureEnvironment(node->as<AwaitExpression>()->argument);
    case NodeType::TemplateLiteral:
      return anyCaptures(node->as<TemplateLiteral>()->expressions);
    case NodeType::TaggedTemplateExpression: {
      auto* n = node->as<TaggedTemplateExpression>();
      return mayCaptureEnvironment(n->tag) || mayCaptureEnvironment(n->quasi);
    }
    case NodeType::SpreadElement:
      return mayCaptureEnvironment(node->as<SpreadElement>()->argument);
    case NodeType::ChainExpression:
      return mayCaptureEnvironment(node->as<ChainExpression>()->expression);
    case NodeType::ObjectPattern:
      return anyCaptures(node->as<ObjectPattern>()->properties);
    case NodeType::ArrayPattern:
      return anyCaptures(node->as<ArrayPattern>()->elements);
    case NodeType::RestElement:
      return mayCaptureEnvironment(node->as<RestElement>()->argument);
    case NodeType::AssignmentPattern: {
      auto* n = node->as<AssignmentPattern>();
      return mayCaptureEnvironment(n->left) || mayCaptureEnvironment(n->right);
    }
    default:
      // Functions, classes, and anything not listed above.
      return true;
  }
}

unsigned gcStressInterval = 0;

void stressCollect() {
  thread_local unsigned count = 0;
  if (++count % gcStressInterval == 0) {
    heap().collect();
  } else {
    heap().maybeCollect();
  }
}

bool loopMayCaptureEnvironment(const Node* loop) {
  // interpCache: nullptr until analysed, then 1 (no capture) or 2 (may capture).
  if (!loop->interpCache) loop->interpCache = reinterpret_cast<void*>(mayCaptureEnvironment(loop) ? 2 : 1);
  return loop->interpCache == reinterpret_cast<void*>(2);
}

}  // namespace snowman::interp
