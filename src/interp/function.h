// ECMAScript function objects (10.2) and the class element records they carry.
#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "interp/environment.h"
#include "parser/ast.h"
#include "runtime/function.h"
#include "runtime/property.h"

namespace snowman::interp {

// A [[Fields]] element (ClassFieldDefinition Record): the field's name is a property key or a Private Name.
struct ClassFieldDefinition {
  std::optional<PropertyKey> key;
  PrivateName* privateName = nullptr;
  Object* initializer = nullptr;  // an ECMAScriptFunction, or nullptr when the field has no initializer
  // A class static block (ClassStaticBlockDefinition Record) in a class's static elements list.
  bool isStaticBlock = false;
};

// PrivateElement (6.2.10).
struct PrivateElement {
  enum class Kind : uint8_t { Field, Method, Accessor };
  PrivateName* key = nullptr;
  Kind kind = Kind::Field;
  Value value;   // Field, Method
  Value getter;  // Accessor: undefined or a function
  Value setter;
};

enum class ThisMode : uint8_t { Lexical, Strict, Global };

// What [[ECMAScriptCode]] is.
enum class CodeKind : uint8_t {
  Function,            // `function`: an ast::Function (params + body)
  FieldInitializer,    // `code`: a class field's initializer expression (ClassFieldDefinition)
  StaticBlock,         // `code`: an ast::StaticBlock (ClassStaticBlockDefinition)
  DefaultConstructor,  // `code`: the ast::Class; behaves as `constructor(...args) { super(...args) }`
};

class ECMAScriptFunction final : public FunctionObject {
 public:
  ECMAScriptFunction(Object* prototype, Realm* realm) : FunctionObject(prototype, realm) {}

  bool isConstructor() const override { return hasConstruct; }
  Result<Value> Call(Value thisArgument, Args args) override;
  Result<Object*> Construct(Args args, Object* newTarget) override;
  void trace(Tracer& tracer) override;
  String* sourceText() const override;

  bool isAsync() const { return function && function->async; }
  bool isGenerator() const { return function && function->generator; }

  CodeKind codeKind = CodeKind::Function;
  const ast::Function* function = nullptr;
  const ast::Node* code = nullptr;
  Environment* environment = nullptr;                // [[Environment]]
  PrivateEnvironment* privateEnvironment = nullptr;  // [[PrivateEnvironment]]
  ThisMode thisMode = ThisMode::Global;
  bool strict = false;
  bool hasConstruct = false;
  bool derived = false;  // [[ConstructorKind]] derived
  bool isClassConstructor = false;
  Object* homeObject = nullptr;
  Cell* scriptOrModule = nullptr;
  std::vector<ClassFieldDefinition> fields;      // [[Fields]]
  std::vector<PrivateElement> privateMethods;    // [[PrivateMethods]]
  Value classFieldInitializerName;               // [[ClassFieldInitializerName]]: empty, or the field name
  std::u16string_view sourceTextView;            // [[SourceText]]
};

// The [[HomeObject]] of an ECMAScript function (nullptr for anything else).
Object* homeObjectOf(Object* function);

}  // namespace snowman::interp
