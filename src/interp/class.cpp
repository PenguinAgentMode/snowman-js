// Classes (15.7): ClassDefinitionEvaluation, class elements, and the private element operations (7.3.26–7.3.32).
#include "interp/interpreter.h"
#include "runtime/builtins.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/vm.h"

namespace snowman::interp {

using namespace ast;

namespace {

// [[PrivateElements]]: the cell an object's privateElements slot holds.
class PrivateElementList final : public Cell {
 public:
  void trace(Tracer& tracer) override {
    for (auto& element : elements) {
      tracer.visit(element.key);
      tracer.visit(element.value);
      tracer.visit(element.getter);
      tracer.visit(element.setter);
    }
  }
  std::vector<PrivateElement> elements;
};

std::vector<PrivateElement>& privateElementsOf(Object* O) {
  if (!O->privateElements) O->privateElements = snowman::make<PrivateElementList>();
  return static_cast<PrivateElementList*>(O->privateElements)->elements;
}

Value privateNameString(PrivateName* name) {
  std::u16string text = u"#";
  text += name->description->view();
  return Value(newString(std::move(text)));
}

}  // namespace

// 7.3.26 PrivateElementFind
PrivateElement* PrivateElementFind(Object* O, PrivateName* P) {
  if (!O->privateElements) return nullptr;
  for (auto& element : static_cast<PrivateElementList*>(O->privateElements)->elements) {
    if (element.key == P) return &element;
  }
  return nullptr;
}

// 7.3.27 PrivateFieldAdd
Result<void> PrivateFieldAdd(Object* O, PrivateName* P, Value value) {
  if (PrivateElementFind(O, P)) return ThrowTypeError("Cannot initialize #" + P->description->toUtf8() + " twice on the same object");
  // nonextensible-applies-to-private: a non-extensible object gets no new private elements either.
  if (!TRY(IsExtensible(O))) return ThrowTypeError("Cannot define private field #" + P->description->toUtf8() + " on a non-extensible object");
  privateElementsOf(O).push_back(PrivateElement{P, PrivateElement::Kind::Field, value, Value(), Value()});
  return {};
}

// 7.3.28 PrivateMethodOrAccessorAdd
Result<void> PrivateMethodOrAccessorAdd(Object* O, const PrivateElement& method) {
  if (PrivateElementFind(O, method.key)) {
    return ThrowTypeError("Cannot initialize #" + method.key->description->toUtf8() + " twice on the same object");
  }
  if (!TRY(IsExtensible(O))) {
    return ThrowTypeError("Cannot define private method #" + method.key->description->toUtf8() + " on a non-extensible object");
  }
  privateElementsOf(O).push_back(method);
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

// 7.3.33 DefineField. A static-block-like record (a static block, or a decorator's extra initializer) is
// called with the receiver instead.
Result<void> DefineField(Object* receiver, const ClassFieldDefinition& field) {
  if (field.isStaticBlock) {
    TRY(Call(Value(field.initializer), Value(receiver)));
    return {};
  }
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
  // Accessor: an auto-accessor, a getter/setter pair (privateElement when the name is private) plus the
  // private storage field it reads and writes.
  enum class Kind { None, Private, Field, StaticBlock, Accessor } kind = Kind::None;
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

// ClassFieldDefinitionEvaluation's initializer: a method of the class whose [[ClassFieldInitializerName]] names
// anonymous functions after the field. nullptr without an initializer.
Object* createFieldInitializer(const Node* value, Object* homeObject, const ElementName& name) {
  if (!value) return nullptr;
  ECMAScriptFunction* initializer = createElementFunction(CodeKind::FieldInitializer, value, homeObject);
  initializer->classFieldInitializerName = name.forFunctionName();
  return initializer;
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
  result.field.initializer = createFieldInitializer(field->value, homeObject, name);
  return result;
}

// A built-in closure of the class machinery (auto-accessor getters and setters, decorator access objects,
// addInitializer, decorated initializers). It carries a Private Name or a property key, and a state cell.
class ClassClosure final : public BuiltinFunction {
 public:
  ClassClosure(Realm* realm, NativeFn fn)
      : BuiltinFunction(realm, fn, realm->intrinsic(Intrinsic::FunctionPrototype), false) {}
  void trace(Tracer& tracer) override {
    BuiltinFunction::trace(tracer);
    tracer.visit(privateName);
    tracer.visit(key);
    tracer.visit(state);
  }
  PrivateName* privateName = nullptr;
  Value key = Value::undefined();  // a String or Symbol
  Cell* state = nullptr;
};

ClassClosure* closureOf(NativeCall& call) { return static_cast<ClassClosure*>(call.callee); }

// CreateBuiltinFunction(fn, length, name, prefix) for a ClassClosure.
ClassClosure* makeClosure(NativeFn fn, int length, const PropertyKey& name, std::u16string_view prefix = {}) {
  auto* F = snowman::make<ClassClosure>(vm().currentRealm(), fn);
  SetFunctionLength(F, length);
  snowman::SetFunctionName(F, name, prefix);
  F->initialName = Value(FunctionNameFromKey(name, prefix));
  return F;
}

// The name SetFunctionName gives a closure for an element ("#x" for a private name).
PropertyKey functionNameKey(const ElementName& name) {
  Value v = name.forFunctionName();
  if (v.isSymbol()) return PropertyKey(v.asSymbol());
  return PropertyKey(v.asString());
}

// An auto-accessor (`accessor x`): a getter and a setter over a new Private Name, its backing storage.
struct AutoAccessor {
  PrivateName* storage = nullptr;
  ClassClosure* getter = nullptr;
  ClassClosure* setter = nullptr;
};

Result<Value> autoAccessorGet(NativeCall& call) {
  if (!call.thisValue.isObject()) return ThrowTypeError("Auto-accessor getter called on a non-object");
  return PrivateGet(call.thisValue.asObject(), closureOf(call)->privateName);
}

Result<Value> autoAccessorSet(NativeCall& call) {
  if (!call.thisValue.isObject()) return ThrowTypeError("Auto-accessor setter called on a non-object");
  TRY(PrivateSet(call.thisValue.asObject(), closureOf(call)->privateName, call.args[0]));
  return Value::undefined();
}

// MakeAutoAccessorGetter / MakeAutoAccessorSetter
AutoAccessor makeAutoAccessor(const ElementName& name) {
  PropertyKey functionName = functionNameKey(name);
  String* description = name.privateName ? name.privateName->description
                                          : atomFor(std::u16string(FunctionNameFromKey(functionName)->view()));
  AutoAccessor accessor;
  accessor.storage = snowman::make<PrivateName>(description);
  accessor.getter = makeClosure(autoAccessorGet, 0, functionName, u"get");
  accessor.getter->privateName = accessor.storage;
  accessor.setter = makeClosure(autoAccessorSet, 1, functionName, u"set");
  accessor.setter->privateName = accessor.storage;
  return accessor;
}

// Defines a public getter and/or setter (enumerable false, configurable true) on homeObject.
Result<void> defineAccessorProperty(Object* homeObject, Value key, Value getter, Value setter) {
  PropertyDescriptor desc;
  if (!getter.isUndefined()) desc.get = getter;
  if (!setter.isUndefined()) desc.set = setter;
  desc.enumerable = false;
  desc.configurable = true;
  return DefinePropertyOrThrow(homeObject, TRY(ToPropertyKey(key)), desc);
}

// ClassElementEvaluation of `accessor x = v` (an undecorated auto-accessor).
Result<ClassElementResult> evaluateAutoAccessor(const AccessorProperty* element, Object* homeObject) {
  ElementName name = TRY(evaluateClassElementName(element->key, element->computed));
  AutoAccessor accessor = makeAutoAccessor(name);
  ClassElementResult result;
  result.kind = ClassElementResult::Kind::Accessor;
  result.field.privateName = accessor.storage;
  result.field.initializer = createFieldInitializer(element->value, homeObject, name);
  if (name.privateName) {
    result.privateElement = PrivateElement{name.privateName, PrivateElement::Kind::Accessor, Value(),
                                           Value(accessor.getter), Value(accessor.setter)};
    return result;
  }
  TRY(defineAccessorProperty(homeObject, name.key, Value(accessor.getter), Value(accessor.setter)));
  return result;
}

Result<ClassElementResult> ClassElementEvaluation(const Node* element, Object* object) {
  switch (element->type) {
    case NodeType::MethodDefinition:
      return evaluateClassMethod(element->as<MethodDefinition>(), object);
    case NodeType::PropertyDefinition:
      return evaluateClassField(element->as<PropertyDefinition>(), object);
    case NodeType::AccessorProperty:
      return evaluateAutoAccessor(element->as<AccessorProperty>(), object);
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
    case NodeType::AccessorProperty:
      return element->as<AccessorProperty>()->isStatic;
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

// The element lists of a class being defined: a cell, so the collector sees the closures they hold.
class ClassElements final : public Cell {
 public:
  void trace(Tracer& tracer) override {
    for (auto* list : {&instancePrivateMethods, &staticPrivateMethods}) {
      for (auto& element : *list) {
        tracer.visit(element.key);
        tracer.visit(element.value);
        tracer.visit(element.getter);
        tracer.visit(element.setter);
      }
    }
    for (auto* list : {&instanceFields, &staticElements}) {
      for (auto& field : *list) {
        if (field.key) field.key->trace(tracer);
        tracer.visit(field.privateName);
        tracer.visit(field.initializer);
      }
    }
  }
  std::vector<PrivateElement> instancePrivateMethods;
  std::vector<PrivateElement> staticPrivateMethods;
  std::vector<ClassFieldDefinition> instanceFields;
  std::vector<ClassFieldDefinition> staticElements;
};

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

// ---- Decorators (proposal-decorators) ----

// The initializers a decorator context's addInitializer appends to.
class InitializerList final : public Cell {
 public:
  void trace(Tracer& tracer) override { tracer.visitAll(values); }
  std::vector<Value> values;
};

// A decorator call's decorationState.[[Finished]], and the list its addInitializer appends to.
class DecorationState final : public Cell {
 public:
  explicit DecorationState(InitializerList* target) : target(target) {}
  void trace(Tracer& tracer) override { tracer.visit(target); }
  InitializerList* target;
  bool finished = false;
};

enum class ElementKind : uint8_t { Method, Getter, Setter, Accessor, Field, StaticBlock };

bool isMethodLike(ElementKind kind) { return kind != ElementKind::Field && kind != ElementKind::StaticBlock; }

// A ClassElementDefinition Record of a decorated class.
struct ElementRecord {
  ElementKind kind = ElementKind::Field;
  bool isStatic = false;
  ElementName name;
  Value value = Value::undefined();   // Method
  Value getter = Value::undefined();  // Getter, Accessor
  Value setter = Value::undefined();  // Setter, Accessor
  PrivateName* storage = nullptr;     // Accessor: the backing storage
  Object* initializer = nullptr;      // Field, Accessor (nullable), StaticBlock
  std::vector<Value> decorators;      // the DecoratorDefinition Records' [[Decorator]]s, in source order
  std::vector<Value> receivers;       // ... and their [[Receiver]]s
  std::vector<Value> initializers;    // Field, Accessor: the initializers the decorators returned
  InitializerList* extraInitializers = nullptr;  // Field, Accessor
};

// A decorated class being defined: a cell, so the collector sees what it holds.
class DecoratedClass final : public Cell {
 public:
  void trace(Tracer& tracer) override {
    for (auto& element : elements) {
      tracer.visit(element.name.key);
      tracer.visit(element.name.privateName);
      tracer.visit(element.value);
      tracer.visit(element.getter);
      tracer.visit(element.setter);
      tracer.visit(element.storage);
      tracer.visit(element.initializer);
      tracer.visitAll(element.decorators);
      tracer.visitAll(element.receivers);
      tracer.visitAll(element.initializers);
      tracer.visit(element.extraInitializers);
    }
    tracer.visitAll(classDecorators);
    tracer.visitAll(classReceivers);
    tracer.visit(staticMethodExtraInitializers);
    tracer.visit(instanceMethodExtraInitializers);
    tracer.visit(classExtraInitializers);
  }
  std::vector<ElementRecord> elements;
  std::vector<Value> classDecorators;
  std::vector<Value> classReceivers;
  // Set right after construction (allocating while this cell is being made would leave them untraced).
  InitializerList* staticMethodExtraInitializers = nullptr;
  InitializerList* instanceMethodExtraInitializers = nullptr;
  InitializerList* classExtraInitializers = nullptr;
};

const std::vector<Decorator*>* elementDecorators(const Node* element) {
  switch (element->type) {
    case NodeType::MethodDefinition:
      return &element->as<MethodDefinition>()->decorators;
    case NodeType::PropertyDefinition:
      return &element->as<PropertyDefinition>()->decorators;
    case NodeType::AccessorProperty:
      return &element->as<AccessorProperty>()->decorators;
    default:
      return nullptr;
  }
}

bool hasDecorators(const Class* node) {
  if (!node->decorators.empty()) return true;
  for (const Node* element : node->body->body) {
    const std::vector<Decorator*>* decorators = elementDecorators(element);
    if (decorators && !decorators->empty()) return true;
  }
  return false;
}

// DecoratorListEvaluation: each decorator's value, and its receiver (the base of `@a.b`, else undefined).
Result<void> evaluateDecorators(const std::vector<Decorator*>& list, std::vector<Value>& decorators,
                                std::vector<Value>& receivers) {
  for (const Decorator* decorator : list) {
    Reference ref = TRY(evaluateReference(decorator->expression));
    Value value = TRY(GetValue(ref));
    decorators.push_back(value);
    receivers.push_back(ref.isPropertyReference() ? GetThisValue(ref) : Value::undefined());
  }
  return {};
}

// ClassElementEvaluation for a decorated class: the element's decorators, then its name and closures.
// Nothing is defined yet.
Result<void> evaluateDecoratedElement(DecoratedClass* state, const Node* element, Object* homeObject) {
  state->elements.emplace_back();
  ElementRecord& record = state->elements.back();  // only this call appends to state->elements meanwhile
  record.isStatic = isStaticElement(element);
  if (const std::vector<Decorator*>* decorators = elementDecorators(element)) {
    TRY(evaluateDecorators(*decorators, record.decorators, record.receivers));
  }
  switch (element->type) {
    case NodeType::MethodDefinition: {
      auto* method = element->as<MethodDefinition>();
      record.name = TRY(evaluateClassElementName(method->key, method->computed));
      ECMAScriptFunction* closure = TRY(DefineMethod(method->value, homeObject));
      if (method->kind == MethodKind::Method) {
        record.kind = ElementKind::Method;
        SetFunctionName(closure, record.name.forFunctionName());
        record.value = Value(closure);
      } else if (method->kind == MethodKind::Get) {
        record.kind = ElementKind::Getter;
        SetFunctionName(closure, record.name.forFunctionName(), u"get");
        record.getter = Value(closure);
      } else {
        record.kind = ElementKind::Setter;
        SetFunctionName(closure, record.name.forFunctionName(), u"set");
        record.setter = Value(closure);
      }
      break;
    }
    case NodeType::PropertyDefinition: {
      auto* field = element->as<PropertyDefinition>();
      record.kind = ElementKind::Field;
      record.name = TRY(evaluateClassElementName(field->key, field->computed));
      record.initializer = createFieldInitializer(field->value, homeObject, record.name);
      record.extraInitializers = snowman::make<InitializerList>();
      break;
    }
    case NodeType::AccessorProperty: {
      auto* accessorNode = element->as<AccessorProperty>();
      record.kind = ElementKind::Accessor;
      record.name = TRY(evaluateClassElementName(accessorNode->key, accessorNode->computed));
      AutoAccessor accessor = makeAutoAccessor(record.name);
      record.storage = accessor.storage;
      record.getter = Value(accessor.getter);
      record.setter = Value(accessor.setter);
      record.initializer = createFieldInitializer(accessorNode->value, homeObject, record.name);
      record.extraInitializers = snowman::make<InitializerList>();
      break;
    }
    case NodeType::StaticBlock:
      record.kind = ElementKind::StaticBlock;
      record.initializer = createElementFunction(CodeKind::StaticBlock, element, homeObject);
      break;
    default:
      state->elements.pop_back();
      break;
  }
  return {};
}

Result<Object*> receiverObject(NativeCall& call) {
  Value O = call.args[0];
  if (!O.isObject()) return ThrowTypeError("The decorated element's access functions need an object");
  return O.asObject();
}

// The access object's get(O), set(O, value) and has(O).
Result<Value> accessGet(NativeCall& call) {
  ClassClosure* F = closureOf(call);
  Object* O = TRY(receiverObject(call));
  if (F->privateName) return PrivateGet(O, F->privateName);
  return Get(O, TRY(ToPropertyKey(F->key)));
}

Result<Value> accessSet(NativeCall& call) {
  ClassClosure* F = closureOf(call);
  Object* O = TRY(receiverObject(call));
  if (F->privateName) {
    TRY(PrivateSet(O, F->privateName, call.args[1]));
  } else {
    TRY(Set(O, TRY(ToPropertyKey(F->key)), call.args[1], true));
  }
  return Value::undefined();
}

Result<Value> accessHas(NativeCall& call) {
  ClassClosure* F = closureOf(call);
  Object* O = TRY(receiverObject(call));
  if (F->privateName) return Value::boolean(PrivateElementFind(O, F->privateName) != nullptr);
  return Value::boolean(TRY(HasProperty(O, TRY(ToPropertyKey(F->key)))));
}

// The context's addInitializer(initializer).
Result<Value> addInitializer(NativeCall& call) {
  auto* state = static_cast<DecorationState*>(closureOf(call)->state);
  if (state->finished) return ThrowTypeError("addInitializer can't be called after the decorator has returned");
  Value initializer = call.args[0];
  if (!IsCallable(initializer)) return ThrowTypeError("addInitializer: the initializer is not a function");
  state->target->values.push_back(initializer);
  return Value::undefined();
}

const char16_t* kindName(ElementKind kind) {
  switch (kind) {
    case ElementKind::Method:
      return u"method";
    case ElementKind::Getter:
      return u"getter";
    case ElementKind::Setter:
      return u"setter";
    case ElementKind::Accessor:
      return u"accessor";
    default:
      return u"field";
  }
}

// CreateDecoratorContextObject: for an element (record), or for the class (record nullptr).
Result<Object*> createDecoratorContext(const ElementRecord* record, Value className, DecorationState* state) {
  Object* objectPrototype = vm().currentRealm()->intrinsic(Intrinsic::ObjectPrototype);
  Object* context = OrdinaryObjectCreate(objectPrototype);
  TRY(CreateDataPropertyOrThrow(context, PropertyKey(u"kind"), Value(atom(record ? kindName(record->kind) : u"class"))));
  if (!record) {
    TRY(CreateDataPropertyOrThrow(context, PropertyKey(u"name"), className));
  } else {
    TRY(CreateDataPropertyOrThrow(context, PropertyKey(u"name"), record->name.forFunctionName()));
    TRY(CreateDataPropertyOrThrow(context, PropertyKey(u"static"), Value::boolean(record->isStatic)));
    TRY(CreateDataPropertyOrThrow(context, PropertyKey(u"private"), Value::boolean(record->name.privateName != nullptr)));
    Object* access = OrdinaryObjectCreate(objectPrototype);
    auto addAccess = [&](NativeFn fn, int length, const char16_t* name) -> Result<void> {
      ClassClosure* function = makeClosure(fn, length, PropertyKey(name));
      function->privateName = record->name.privateName;
      function->key = record->name.key;
      return CreateDataPropertyOrThrow(access, PropertyKey(name), Value(function));
    };
    ElementKind kind = record->kind;
    if (kind != ElementKind::Setter) TRY(addAccess(accessGet, 1, u"get"));
    if (kind == ElementKind::Setter || kind == ElementKind::Field || kind == ElementKind::Accessor) {
      TRY(addAccess(accessSet, 2, u"set"));
    }
    TRY(addAccess(accessHas, 1, u"has"));
    TRY(CreateDataPropertyOrThrow(context, PropertyKey(u"access"), Value(access)));
  }
  ClassClosure* add = makeClosure(addInitializer, 1, PropertyKey(u"addInitializer"));
  add->state = state;
  TRY(CreateDataPropertyOrThrow(context, PropertyKey(u"addInitializer"), Value(add)));
  return context;
}

// ApplyDecoratorsToElementDefinition: calls the element's decorators, innermost (last) first.
// methodExtraInitializers receives a method's, getter's or setter's addInitializer calls.
Result<void> applyElementDecorators(DecoratedClass* state, size_t index, InitializerList* methodExtraInitializers) {
  ElementRecord& record = state->elements[index];  // the element list doesn't change any more
  bool fieldLike = record.kind == ElementKind::Field || record.kind == ElementKind::Accessor;
  for (size_t i = record.decorators.size(); i-- > 0;) {
    auto* decorationState =
        snowman::make<DecorationState>(fieldLike ? record.extraInitializers : methodExtraInitializers);
    Object* context = TRY(createDecoratorContext(&record, Value::undefined(), decorationState));
    Value value = Value::undefined();
    switch (record.kind) {
      case ElementKind::Method:
        value = record.value;
        break;
      case ElementKind::Getter:
        value = record.getter;
        break;
      case ElementKind::Setter:
        value = record.setter;
        break;
      case ElementKind::Accessor: {
        Object* pair = OrdinaryObjectCreate(vm().currentRealm()->intrinsic(Intrinsic::ObjectPrototype));
        TRY(CreateDataPropertyOrThrow(pair, PropertyKey(u"get"), record.getter));
        TRY(CreateDataPropertyOrThrow(pair, PropertyKey(u"set"), record.setter));
        value = Value(pair);
        break;
      }
      default:
        break;
    }
    Value newValue = TRY(Call(record.decorators[i], record.receivers[i], {value, Value(context)}));
    decorationState->finished = true;
    if (newValue.isUndefined()) continue;
    switch (record.kind) {
      case ElementKind::Field:
        if (!IsCallable(newValue)) return ThrowTypeError("A field decorator must return a function or undefined");
        record.initializers.push_back(newValue);
        break;
      case ElementKind::Accessor: {
        if (!newValue.isObject()) return ThrowTypeError("An accessor decorator must return an object or undefined");
        Object* result = newValue.asObject();
        Value getter = TRY(Get(result, PropertyKey(u"get")));
        if (IsCallable(getter)) {
          record.getter = getter;
        } else if (!getter.isUndefined()) {
          return ThrowTypeError("An accessor decorator's get must be a function or undefined");
        }
        Value setter = TRY(Get(result, PropertyKey(u"set")));
        if (IsCallable(setter)) {
          record.setter = setter;
        } else if (!setter.isUndefined()) {
          return ThrowTypeError("An accessor decorator's set must be a function or undefined");
        }
        Value init = TRY(Get(result, PropertyKey(u"init")));
        if (IsCallable(init)) {
          record.initializers.push_back(init);
        } else if (!init.isUndefined()) {
          return ThrowTypeError("An accessor decorator's init must be a function or undefined");
        }
        break;
      }
      default:
        if (!IsCallable(newValue)) return ThrowTypeError("A method decorator must return a function or undefined");
        if (record.kind == ElementKind::Method) {
          record.value = newValue;
        } else if (record.kind == ElementKind::Getter) {
          record.getter = newValue;
        } else {
          record.setter = newValue;
        }
        break;
    }
  }
  return {};
}

// Defines a decorated method, getter, setter or auto-accessor on homeObject (or records its private element).
Result<void> defineDecoratedMethod(const ElementRecord& record, Object* homeObject, ClassElements* lists) {
  if (record.name.privateName) {
    PrivateElement element = record.kind == ElementKind::Method
        ? PrivateElement{record.name.privateName, PrivateElement::Kind::Method, record.value, Value(), Value()}
        : PrivateElement{record.name.privateName, PrivateElement::Kind::Accessor, Value(), record.getter, record.setter};
    addPrivateMethod(record.isStatic ? lists->staticPrivateMethods : lists->instancePrivateMethods, element);
    return {};
  }
  if (record.kind == ElementKind::Method) return DefineMethodProperty(homeObject, record.name.key, record.value.asObject(), false);
  return defineAccessorProperty(homeObject, record.name.key, record.getter, record.setter);
}

// A field's initializer followed by the initializers its decorators returned: each gets the value so far.
Result<Value> runDecoratedInitializers(NativeCall& call) {
  const std::vector<Value>& slots = call.callee->slots;
  Value value = Value::undefined();
  if (!slots[0].isUndefined()) value = TRY(Call(slots[0], call.thisValue));
  for (size_t i = 1; i < slots.size(); ++i) value = TRY(Call(slots[i], call.thisValue, {value}));
  return value;
}

Object* decoratedInitializer(const ElementRecord& record) {
  if (record.initializers.empty()) return record.initializer;
  auto* F = snowman::make<ClassClosure>(vm().currentRealm(), runDecoratedInitializers);
  F->slots.push_back(record.initializer ? Value(record.initializer) : Value::undefined());
  for (const Value& initializer : record.initializers) F->slots.push_back(initializer);
  return F;
}

// An extra initializer, run like a static block with the instance (or class) as its receiver.
ClassFieldDefinition extraInitializerRecord(Value initializer) {
  ClassFieldDefinition record;
  record.initializer = initializer.asObject();
  record.isStaticBlock = true;
  return record;
}

// Decorates the evaluated elements of a decorated class, defines its methods, and fills the field lists:
// methods, getters, setters and auto-accessors (static ones, then instance ones), then fields (likewise).
Result<void> decorateAndDefineElements(DecoratedClass* state, Object* F, Object* proto, ClassElements* lists) {
  for (bool isStatic : {true, false}) {
    InitializerList* extra = isStatic ? state->staticMethodExtraInitializers : state->instanceMethodExtraInitializers;
    for (size_t i = 0; i < state->elements.size(); ++i) {
      if (state->elements[i].isStatic != isStatic || !isMethodLike(state->elements[i].kind)) continue;
      TRY(applyElementDecorators(state, i, extra));
      TRY(defineDecoratedMethod(state->elements[i], isStatic ? F : proto, lists));
    }
  }
  for (bool isStatic : {true, false}) {
    for (size_t i = 0; i < state->elements.size(); ++i) {
      if (state->elements[i].isStatic != isStatic || state->elements[i].kind != ElementKind::Field) continue;
      TRY(applyElementDecorators(state, i, nullptr));
    }
  }
  // A method's extra initializers run before the fields (on instances: after the private methods are added).
  for (const Value& initializer : state->staticMethodExtraInitializers->values) {
    lists->staticElements.push_back(extraInitializerRecord(initializer));
  }
  for (const Value& initializer : state->instanceMethodExtraInitializers->values) {
    lists->instanceFields.push_back(extraInitializerRecord(initializer));
  }
  for (const ElementRecord& record : state->elements) {
    if (record.kind == ElementKind::StaticBlock) {
      ClassFieldDefinition block;
      block.initializer = record.initializer;
      block.isStaticBlock = true;
      lists->staticElements.push_back(block);
      continue;
    }
    if (isMethodLike(record.kind) && record.kind != ElementKind::Accessor) continue;
    ClassFieldDefinition field;
    if (record.kind == ElementKind::Accessor) {
      field.privateName = record.storage;
    } else if (record.name.privateName) {
      field.privateName = record.name.privateName;
    } else {
      field.key = TRY(ToPropertyKey(record.name.key));
    }
    field.initializer = decoratedInitializer(record);
    std::vector<ClassFieldDefinition>& list = record.isStatic ? lists->staticElements : lists->instanceFields;
    list.push_back(field);
    // A field's (or auto-accessor's) extra initializers run right after it is defined.
    for (const Value& initializer : record.extraInitializers->values) list.push_back(extraInitializerRecord(initializer));
  }
  return {};
}

// ApplyDecoratorsToClassDefinition: the class decorators, innermost (last) first; each may replace the class.
Result<Value> applyClassDecorators(DecoratedClass* state, Value classDef, Value className) {
  for (size_t i = state->classDecorators.size(); i-- > 0;) {
    auto* decorationState = snowman::make<DecorationState>(state->classExtraInitializers);
    Object* context = TRY(createDecoratorContext(nullptr, className, decorationState));
    Value newDef = TRY(Call(state->classDecorators[i], state->classReceivers[i], {classDef, Value(context)}));
    decorationState->finished = true;
    if (newDef.isUndefined()) continue;
    if (!IsCallable(newDef)) return ThrowTypeError("A class decorator must return a function or undefined");
    classDef = newDef;
  }
  return classDef;
}

}  // namespace

// 15.7.14 ClassDefinitionEvaluation
Result<Value> ClassDefinitionEvaluation(const Class* node, String* classBinding, Value className) {
  // Decorators: the class's are evaluated first, in the surrounding scope.
  DecoratedClass* decorated = nullptr;
  if (hasDecorators(node)) {
    decorated = snowman::make<DecoratedClass>();
    decorated->staticMethodExtraInitializers = snowman::make<InitializerList>();
    decorated->instanceMethodExtraInitializers = snowman::make<InitializerList>();
    decorated->classExtraInitializers = snowman::make<InitializerList>();
    TRY(evaluateDecorators(node->decorators, decorated->classDecorators, decorated->classReceivers));
  }
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

  auto* elements = snowman::make<ClassElements>();
  auto& instancePrivateMethods = elements->instancePrivateMethods;
  auto& staticPrivateMethods = elements->staticPrivateMethods;
  auto& instanceFields = elements->instanceFields;
  auto& staticElements = elements->staticElements;
  for (const Node* e : node->body->body) {
    if (e->type == NodeType::MethodDefinition && e->as<MethodDefinition>()->kind == MethodKind::Constructor) continue;
    bool isStatic = isStaticElement(e);
    Object* homeObject = isStatic ? static_cast<Object*>(F) : proto;
    if (decorated) {
      TRY(evaluateDecoratedElement(decorated, e, homeObject));
      continue;
    }
    ClassElementResult element = TRY(ClassElementEvaluation(e, homeObject));
    switch (element.kind) {
      case ClassElementResult::Kind::Private:
        addPrivateMethod(isStatic ? staticPrivateMethods : instancePrivateMethods, element.privateElement);
        break;
      case ClassElementResult::Kind::Field:
        (isStatic ? staticElements : instanceFields).push_back(element.field);
        break;
      case ClassElementResult::Kind::Accessor:
        if (element.privateElement.key) {
          addPrivateMethod(isStatic ? staticPrivateMethods : instancePrivateMethods, element.privateElement);
        }
        (isStatic ? staticElements : instanceFields).push_back(element.field);
        break;
      case ClassElementResult::Kind::StaticBlock:
        staticElements.push_back(element.field);
        break;
      case ClassElementResult::Kind::None:
        break;
    }
  }
  if (decorated) TRY(decorateAndDefineElements(decorated, F, proto, elements));
  context->lexicalEnvironment = env;
  F->privateMethods = std::move(instancePrivateMethods);
  F->fields = std::move(instanceFields);
  Value classValue = Value(F);
  if (decorated) classValue = TRY(applyClassDecorators(decorated, classValue, className));
  if (classBinding) TRY(classEnv->InitializeBinding(classBinding, classValue));
  for (const PrivateElement& method : staticPrivateMethods) TRY(PrivateMethodOrAccessorAdd(F, method));
  for (const ClassFieldDefinition& elementRecord : staticElements) TRY(DefineField(F, elementRecord));
  if (decorated) {
    // Class decorators' extra initializers, with the (possibly replaced) class. The list is final by now.
    const std::vector<Value>& initializers = decorated->classExtraInitializers->values;
    for (size_t i = 0; i < initializers.size(); ++i) TRY(Call(initializers[i], classValue));
  }
  return classValue;
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
