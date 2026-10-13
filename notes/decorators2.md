# decorators2 handoff

- Branch `ws/decorators2`, on main 53d4d86. Started from temporal2's parser WIP (ws/decorators fb2d287, cherry-picked
  alone; its parent 64a2771 "reuse exited Scopes" is NOT included, it is a separate change).
- Files: src/parser/{lexer.h,lexer.cpp,ast.h,dump.cpp,expressions.cpp,parser_impl.h,statements.cpp}, src/interp/class.cpp.

## State

- Parser (fb2d287): `@` token (T::At); ast::Decorator; `decorators` on Class / MethodDefinition / PropertyDefinition /
  AccessorProperty; `accessor [no LineTerminator] ClassElementName` (ast::AccessorProperty); decorators before
  `class`, `export @dec class`, `@dec export class`, `export default @dec class`, class expressions.
  Early errors: decorators on a constructor / static block; `@foo().bar`, `@a[0]`, `@a?.b`, `@this` rejected.
- Runtime (class.cpp):
  - Auto-accessors (undecorated classes too): getter/setter are ClassClosure built-ins over a fresh PrivateName
    (the storage); the storage is a private field in the fields list (document order). Public: accessor property
    on proto/F (non-enumerable, configurable); private: a PrivateElement accessor.
  - Decorated classes (any decorator on the class or an element) take a separate path: elements evaluated first
    (decorators, then key, then closures; nothing defined), then decorators applied innermost-first and methods
    defined: static methods/getters/setters/accessors, instance ones, then static fields, instance fields; then
    class decorators (may replace the class; the inner binding gets the replacement), then static elements, then
    class extra initializers. Field/accessor addInitializer callbacks run right after that field is defined;
    method ones run before the fields (instance: after private methods are added). Decorators are called with
    their natural receiver (`@a.b` → this = a).
  - Context: kind, name, static, private, access {get, set, has}, addInitializer. No `metadata` (the engine has
    no Symbol.metadata; test262 at our pin has no decorator-metadata tests).
  - Undecorated classes keep the old path (only the Accessor case added), so ordering is unchanged there.

## Numbers

- 27 decorator-feature files: 24 failing at 317949e37 → all 27 pass (test/language/{statements,expressions}/class/
  decorator 18+... , grammar-field-accessor x2, staging/decorators 3).

## Gotchas

- Don't use `&` inside a background bash call: the PID namespace kills it when the call returns.
- PrivateName doesn't trace its description: use atomFor() strings.
- DecoratedClass must not allocate in its constructor (members are set after make<>).

## Next

- Full gate on the pushed sha (see the merge request in the lead's queue).
