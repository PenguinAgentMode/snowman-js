// Classes (15.7): ClassDefinitionEvaluation, class elements, and the private element operations (7.3.26–7.3.32).
#include <unordered_map>

#include "interp/interpreter.h"
#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/vm.h"

namespace snowman::interp {

using namespace ast;

namespace {

// [[PrivateElements]] of every object that has any. A side table until Object carries the slot.
class PrivateElementTable final : public RootProvider {
 public:
  std::unordered_map<Object*, std::vector<PrivateElement>> elements;

  void traceRoots(Tracer& tracer) override {
    for (auto& [object, list] : elements) {
      tracer.visit(object);
      for (auto& element : list) {
        tracer.visit(element.key);
        tracer.visit(element.value);
        tracer.visit(element.getter);
        tracer.visit(element.setter);
      }
    }
  }
};

PrivateElementTable& privateTable() {
  static auto* table = new PrivateElementTable();
  return *table;
}

Value privateNameString(PrivateName* name) {
  std::u16string text = u"#";
  text += name->description->view();
  return Value(newString(std::move(text)));
}

}  // namespace

// 7.3.26 PrivateElementFind
PrivateElement* PrivateElementFind(Object* O, PrivateName* P) {
  auto& table = privateTable().elements;
  auto it = table.find(O);
  if (it == table.end()) return nullptr;
  for (auto& element : it->second) {
    if (element.key == P) return &element;
  }
  return nullptr;
}

// 7.3.27 PrivateFieldAdd
Result<void> PrivateFieldAdd(Object* O, PrivateName* P, Value value) {
  if (PrivateElementFind(O, P)) return ThrowTypeError("Cannot initialize #" + P->description->toUtf8() + " twice on the same object");
  privateTable().elements[O].push_back(PrivateElement{P, PrivateElement::Kind::Field, value, Value(), Value()});
  return {};
}

// 7.3.28 PrivateMethodOrAccessorAdd
Result<void> PrivateMethodOrAccessorAdd(Object* O, const PrivateElement& method) {
  if (PrivateElementFind(O, method.key)) {
    return ThrowTypeError("Cannot initialize #" + method.key->description->toUtf8() + " twice on the same object");
  }
  privateTable().elements[O].push_back(method);
  return {};
}

// 7.3.31 PrivateGet
Result<Value> PrivateGet(Object* O, PrivateName* P) {
  PrivateElement* entry = PrivateElementFind(O, P);
  if (!entry) return ThrowTypeError("Cannot read private member #" + P->description->toUtf8() + " from an object whose class did not declare it");
  if (entry->kind != PrivateElement::Kind::Accessor) return entry->value;
  Value getter = entry->getter;
  if (getter.isUndefined()) return ThrowTypeError("'#" + P->description->toUtf8() + "' was defined without a getter");
  return Call(getter, Value(O));
}

// 7.3.32 PrivateSet
Result<void> PrivateSet(Object* O, PrivateName* P, Value value) {
  PrivateElement* entry = PrivateElementFind(O, P);
  if (!entry) return ThrowTypeError("Cannot write private member #" + P->description->toUtf8() + " to an object whose class did not declare it");
  if (entry->kind == PrivateElement::Kind::Field) {
    entry->value = value;
    return {};
  }
  if (entry->kind == PrivateElement::Kind::Method) {
    return ThrowTypeError("Private method #" + P->description->toUtf8() + " is not writable");
  }
  Value setter = entry->setter;
  if (setter.isUndefined()) return ThrowTypeError("'#" + P->description->toUtf8() + "' was defined without a setter");
  TRY(Call(setter, Value(O), {value}));
  return {};
}

namespace {

// 7.3.33 DefineField
Result<void> DefineField(Object* receiver, const ClassFieldDefinition& field) {
  Value initValue = Value::undefined();
  if (field.initializer) initValue = TRY(Call(Value(field.initializer), Value(receiver)));
  if (field.privateName) return PrivateFieldAdd(receiver, field.privateName, initValue);
  return CreateDataPropertyOrThrow(receiver, *field.key, initValue);
}

// A class element's name: a property key value, or a Private Name.
struct ElementName {
  Value key;  // String or Symbol when not private
  PrivateName* privateName = nullptr;
  Value forFunctionName() const { return privateName ? privateNameString(privateName) : key; }
};

Result<ElementName> evaluateClassElementName(const Node* key, bool computed) {
  if (key->type == NodeType::PrivateIdentifier) {
    String* name = atomFor(key->as<PrivateIdentifier>()->name);
    return ElementName{Value(), runningContext()->privateEnvironment->findOwn(name)};
  }
  return ElementName{TRY(evaluatePropertyKey(key, computed)), nullptr};
}

// The result of ClassElementEvaluation.
struct ClassElementResult {
  enum class Kind { None, Private, Field, StaticBlock } kind = Kind::None;
  PrivateElement privateElement;
  ClassFieldDefinition field;
};

// MethodDefinitionEvaluation (enumerable false) for a class method, getter or setter.
Result<ClassElementResult> evaluateClassMethod(const MethodDefinition* method, Object* homeObject) {
  ElementName name = TRY(evaluateClassElementName(method->key, method->computed));
  ECMAScriptFunction* closure = TRY(DefineMethod(method->value, homeObject));
  ClassElementResult result;
  if (method->kind == MethodKind::Method) {
    SetFunctionName(closure, name.forFunctionName());
    if (name.privateName) {
      result.kind = ClassElementResult::Kind::Private;
      result.privateElement = PrivateElement{name.privateName, PrivateElement::Kind::Method, Value(closure), Value(), Value()};
      return result;
    }
    TRY(DefineMethodProperty(homeObject, name.key, closure, false));
    return result;
  }
  bool getter = method->kind == MethodKind::Get;
  SetFunctionName(closure, name.forFunctionName(), getter ? u"get" : u"set");
  if (name.privateName) {
    result.kind = ClassElementResult::Kind::Private;
    result.privateElement = PrivateElement{name.privateName, PrivateElement::Kind::Accessor, Value(),
                                           getter ? Value(closure) : Value::undefined(),
                                           getter ? Value::undefined() : Value(closure)};
    return result;
  }
  PropertyDescriptor desc;
  if (getter) {
    desc.get = Value(closure);
  } else {
    desc.set = Value(closure);
  }
  desc.enumerable = false;
  desc.configurable = true;
  TRY(DefinePropertyOrThrow(homeObject, TRY(ToPropertyKey(name.key)), desc));
  return result;
}

ECMAScriptFunction* createElementFunction(CodeKind kind, const Node* code, Object* homeObject) {
  InterpContext* context = runningContext();
  Realm* realm = vm().currentRealm();
  auto* F = snowman::make<ECMAScriptFunction>(realm->intrinsic(Intrinsic::FunctionPrototype), realm);
  F->codeKind = kind;
  F->code = code;
  F->strict = true;
  F->thisMode = ThisMode::Strict;
  F->environment = context->lexicalEnvironment;
  F->privateEnvironment = context->privateEnvironment;
  F->scriptOrModule = GetActiveScriptOrModule();
  F->classFieldInitializerName = Value::empty();
  F->homeObject = homeObject;
  SetFunctionLength(F, 0);
  return F;
}

// ClassFieldDefinitionEvaluation
Result<ClassElementResult> evaluateClassField(const PropertyDefinition* field, Object* homeObject) {
  ElementName name = TRY(evaluateClassElementName(field->key, field->computed));
  ClassElementResult result;
  result.kind = ClassElementResult::Kind::Field;
  if (name.privateName) {
    result.field.privateName = name.privateName;
  } else {
    result.field.key = TRY(ToPropertyKey(name.key));
  }
  if (field->value) {
    ECMAScriptFunction* initializer = createElementFunction(CodeKind::FieldInitializer, field->value, homeObject);
    initializer->classFieldInitializerName = name.forFunctionName();
    result.field.initializer = initializer;
  }
  return result;
}

Result<ClassElementResult> ClassElementEvaluation(const Node* element, Object* object) {
  switch (element->type) {
    case NodeType::MethodDefinition:
      return evaluateClassMethod(element->as<MethodDefinition>(), object);
    case NodeType::PropertyDefinition:
      return evaluateClassField(element->as<PropertyDefinition>(), object);
    case NodeType::StaticBlock: {
      ClassElementResult result;
      result.kind = ClassElementResult::Kind::StaticBlock;
      result.field.isStaticBlock = true;
      result.field.initializer = createElementFunction(CodeKind::StaticBlock, element, object);
      return result;
    }
    default:
      return ClassElementResult{};
  }
}

bool isStaticElement(const Node* element) {
  switch (element->type) {
    case NodeType::MethodDefinition:
      return element->as<MethodDefinition>()->isStatic;
    case NodeType::PropertyDefinition:
      return element->as<PropertyDefinition>()->isStatic;
    case NodeType::StaticBlock:
      return true;
    default:
      return false;
  }
}

void addPrivateMethod(std::vector<PrivateElement>& container, const PrivateElement& element) {
  for (auto& pe : container) {
    if (pe.key != element.key) continue;
    if (element.getter.isUndefined()) {
      pe = PrivateElement{element.key, PrivateElement::Kind::Accessor, Value(), pe.getter, element.setter};
    } else {
      pe = PrivateElement{element.key, PrivateElement::Kind::Accessor, Value(), element.getter, pe.setter};
    }
    return;
  }
  container.push_back(element);
}

// Restores the running context's environments and strictness when the class definition ends.
class ClassScope {
 public:
  ClassScope()
      : context_(runningContext()),
        env_(context_->lexicalEnvironment),
        privateEnv_(context_->privateEnvironment),
        strict_(context_->strict) {
    context_->strict = true;
  }
  ~ClassScope() {
    context_->lexicalEnvironment = env_;
    context_->privateEnvironment = privateEnv_;
    context_->strict = strict_;
  }
  ClassScope(const ClassScope&) = delete;
  ClassScope& operator=(const ClassScope&) = delete;

  InterpContext* context() const { return context_; }
  Environment* env() const { return env_; }
  PrivateEnvironment* privateEnv() const { return privateEnv_; }

 private:
  InterpContext* context_;
  Environment* env_;
  PrivateEnvironment* privateEnv_;
  bool strict_;
};

}  // namespace

// 15.7.14 ClassDefinitionEvaluation
Result<Value> ClassDefinitionEvaluation(const Class* node, String* classBinding, Value className) {
  ClassScope scope;
  InterpContext* context = scope.context();
  Environment* env = scope.env();
  Realm* realm = vm().currentRealm();

  auto* classEnv = snowman::make<DeclarativeEnvironment>(env);
  if (classBinding) TRY(classEnv->CreateImmutableBinding(classBinding, true));
  PrivateEnvironment* outerPrivateEnvironment = scope.privateEnv();
  auto* classPrivateEnvironment = snowman::make<PrivateEnvironment>(outerPrivateEnvironment);
  for (const auto& dn : node->privateBoundIdentifiers) {
    classPrivateEnvironment->names.push_back(snowman::make<PrivateName>(atomFor(dn)));
  }

  Object* protoParent;
  Object* constructorParent;
  if (!node->superClass) {
    protoParent = realm->intrinsic(Intrinsic::ObjectPrototype);
    constructorParent = realm->intrinsic(Intrinsic::FunctionPrototype);
  } else {
    context->lexicalEnvironment = classEnv;
    Result<Value> superclassResult = evaluate(node->superClass);
    context->lexicalEnvironment = env;
    Value superclass = TRY(std::move(superclassResult));
    if (superclass.isNull()) {
      protoParent = nullptr;
      constructorParent = realm->intrinsic(Intrinsic::FunctionPrototype);
    } else if (!IsConstructor(superclass)) {
      return ThrowTypeError("Class extends value is not a constructor or null");
    } else {
      Value parent = TRY(Get(superclass.asObject(), PropertyKey(u"prototype")));
      if (!parent.isObject() && !parent.isNull()) {
        return ThrowTypeError("Class extends value does not have a valid prototype property");
      }
      protoParent = parent.isNull() ? nullptr : parent.asObject();
      constructorParent = superclass.asObject();
    }
  }
  Object* proto = OrdinaryObjectCreate(protoParent);
  context->lexicalEnvironment = classEnv;
  context->privateEnvironment = classPrivateEnvironment;

  ECMAScriptFunction* F;
  if (!node->constructor) {
    F = snowman::make<ECMAScriptFunction>(constructorParent, realm);
    F->codeKind = CodeKind::DefaultConstructor;
    F->code = node;
    F->strict = true;
    F->thisMode = ThisMode::Strict;
    F->environment = classEnv;
    F->privateEnvironment = classPrivateEnvironment;
    F->scriptOrModule = GetActiveScriptOrModule();
    F->classFieldInitializerName = Value::empty();
    SetFunctionLength(F, 0);
    SetFunctionName(F, className);
  } else {
    F = TRY(DefineMethod(node->constructor->value, proto, constructorParent));
    MakeClassConstructor(F);
    SetFunctionName(F, className);
  }
  F->sourceTextView = node->sourceText;
  MakeConstructor(F, false, proto);
  if (node->superClass) F->derived = true;
  TRY(DefineMethodProperty(proto, Value(atom(u"constructor")), F, false));

  std::vector<PrivateElement> instancePrivateMethods;
  std::vector<PrivateElement> staticPrivateMethods;
  std::vector<ClassFieldDefinition> instanceFields;
  std::vector<ClassFieldDefinition> staticElements;
  for (const Node* e : node->body->body) {
    if (e->type == NodeType::MethodDefinition && e->as<MethodDefinition>()->kind == MethodKind::Constructor) continue;
    bool isStatic = isStaticElement(e);
    ClassElementResult element = TRY(ClassElementEvaluation(e, isStatic ? static_cast<Object*>(F) : proto));
    switch (element.kind) {
      case ClassElementResult::Kind::Private:
        addPrivateMethod(isStatic ? staticPrivateMethods : instancePrivateMethods, element.privateElement);
        break;
      case ClassElementResult::Kind::Field:
        (isStatic ? staticElements : instanceFields).push_back(element.field);
        break;
      case ClassElementResult::Kind::StaticBlock:
        staticElements.push_back(element.field);
        break;
      case ClassElementResult::Kind::None:
        break;
    }
  }
  context->lexicalEnvironment = env;
  if (classBinding) TRY(classEnv->InitializeBinding(classBinding, Value(F)));
  F->privateMethods = std::move(instancePrivateMethods);
  F->fields = std::move(instanceFields);
  for (const PrivateElement& method : staticPrivateMethods) TRY(PrivateMethodOrAccessorAdd(F, method));
  for (const ClassFieldDefinition& elementRecord : staticElements) {
    if (elementRecord.isStaticBlock) {
      TRY(Call(Value(elementRecord.initializer), Value(F)));
    } else {
      TRY(DefineField(F, elementRecord));
    }
  }
  return Value(F);
}

Result<Value> evaluateClass(const Class* node, Value name) {
  if (node->id) {
    String* className = identifierName(node->id);
    return ClassDefinitionEvaluation(node, className, Value(className));
  }
  if (name.isEmpty()) name = Value(emptyString());
  return ClassDefinitionEvaluation(node, nullptr, name);
}

// 7.3.34 InitializeInstanceElements
Result<void> InitializeInstanceElements(Object* O, ECMAScriptFunction* constructor) {
  for (const PrivateElement& method : constructor->privateMethods) TRY(PrivateMethodOrAccessorAdd(O, method));
  for (const ClassFieldDefinition& field : constructor->fields) TRY(DefineField(O, field));
  return {};
}

}  // namespace snowman::interp
