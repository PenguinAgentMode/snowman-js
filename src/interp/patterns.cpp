// Destructuring: BindingInitialization (8.6.2) and DestructuringAssignmentEvaluation (13.15.5). Both walk the
// same ESTree patterns; they differ in how a target is resolved and stored.
#include "interp/interpreter.h"
#include "runtime/errors.h"
#include "runtime/operations.h"

namespace snowman::interp {

using namespace ast;

namespace {

struct Destructurer {
  bool assignment;           // DestructuringAssignmentEvaluation
  Environment* environment;  // BindingInitialization's environment (nullptr: PutValue)

  // The reference for a non-pattern target (resolved before the value is read).
  Result<Reference> targetReference(const Node* target) {
    if (target->type == NodeType::Identifier) {
      return ResolveBinding(identifierName(target->as<Identifier>()), assignment ? nullptr : environment);
    }
    return evaluateReference(target);
  }

  Result<void> store(Reference& ref, Value value) {
    if (!assignment && environment) return InitializeReferencedBinding(ref, value);
    return PutValue(ref, value);
  }

  static bool isPattern(const Node* node) {
    return node->type == NodeType::ObjectPattern || node->type == NodeType::ArrayPattern;
  }

  Result<void> bind(const Node* pattern, Value value) {
    switch (pattern->type) {
      case NodeType::ObjectPattern:
        return bindObject(pattern->as<ObjectPattern>(), value);
      case NodeType::ArrayPattern:
        return bindArray(pattern->as<ArrayPattern>(), value);
      default: {
        Reference ref = TRY(targetReference(pattern));
        return store(ref, value);
      }
    }
  }

  // An element with an optional initializer whose value comes from `getValue` (after the target is resolved).
  template <class GetValueFn>
  Result<void> bindElement(const Node* element, GetValueFn&& getValue) {
    const Node* target = element;
    const Node* initializer = nullptr;
    if (element->type == NodeType::AssignmentPattern) {
      target = element->as<AssignmentPattern>()->left;
      initializer = element->as<AssignmentPattern>()->right;
    }
    std::optional<Reference> ref;
    if (!isPattern(target)) ref = TRY(targetReference(target));
    Value value = TRY(getValue());
    if (initializer && value.isUndefined()) {
      if (target->type == NodeType::Identifier && !target->parenthesized && IsAnonymousFunctionDefinition(initializer)) {
        value = TRY(NamedEvaluation(initializer, Value(identifierName(target->as<Identifier>()))));
      } else {
        value = TRY(evaluate(initializer));
      }
    }
    if (ref) return store(*ref, value);
    return bind(target, value);
  }

  Result<void> bindObject(const ObjectPattern* pattern, Value value) {
    TRY(RequireObjectCoercible(value));
    std::vector<PropertyKey> excludedNames;
    for (const Node* member : pattern->properties) {
      if (member->type == NodeType::RestElement) {
        Reference ref = TRY(targetReference(member->as<RestElement>()->argument));
        Object* restObj = OrdinaryObjectCreate(vm().currentRealm()->intrinsic(Intrinsic::ObjectPrototype));
        TRY(CopyDataProperties(restObj, value, excludedNames));
        TRY(store(ref, Value(restObj)));
        continue;
      }
      const Property* property = member->as<Property>();
      Value nameValue = TRY(evaluatePropertyKey(property->key, property->computed));
      PropertyKey name = TRY(ToPropertyKey(nameValue));
      excludedNames.push_back(name);
      TRY(bindElement(property->value, [&]() -> Result<Value> { return GetV(value, name); }));
    }
    return {};
  }

  Result<void> iteratorBinding(const ArrayPattern* pattern, IteratorRecord& record) {
    for (const Node* element : pattern->elements) {
      if (!element) {
        if (!record.done) TRY(IteratorStepValue(record));
        continue;
      }
      if (element->type == NodeType::RestElement) {
        const Node* target = element->as<RestElement>()->argument;
        std::optional<Reference> ref;
        if (!isPattern(target)) ref = TRY(targetReference(target));
        Object* A = TRY(ArrayCreate(0));
        uint32_t n = 0;
        while (!record.done) {
          std::optional<Value> next = TRY(IteratorStepValue(record));
          if (!next) break;
          TRY(CreateDataPropertyOrThrow(A, PropertyKey::index(n), *next));
          n++;
        }
        if (ref) return store(*ref, Value(A));
        return bind(target, Value(A));
      }
      TRY(bindElement(element, [&]() -> Result<Value> {
        if (record.done) return Value::undefined();
        std::optional<Value> next = TRY(IteratorStepValue(record));
        return next ? *next : Value::undefined();
      }));
    }
    return {};
  }

  Result<void> bindArray(const ArrayPattern* pattern, Value value) {
    IteratorRecord record = TRY(GetIterator(value, IteratorKind::Sync));
    Result<void> result = iteratorBinding(pattern, record);
    if (record.done) return result;
    // IteratorClose(iteratorRecord, result); the generator return marker counts as a return completion.
    bool marker = result.isThrow() && isReturnMarker(result.thrown());
    Result<void> closed = IteratorClose(record, marker ? Result<void>() : result);
    if (marker && closed.isNormal()) return result;
    return closed;
  }
};

}  // namespace

Result<void> BindingInitialization(const Node* pattern, Value value, Environment* environment) {
  Destructurer destructurer{false, environment};
  return destructurer.bind(pattern, value);
}

Result<void> DestructuringAssignmentEvaluation(const Node* pattern, Value value) {
  Destructurer destructurer{true, nullptr};
  return destructurer.bind(pattern, value);
}

}  // namespace snowman::interp
