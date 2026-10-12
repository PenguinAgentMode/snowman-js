# Interfaces (owned by the project lead; change only through them)

Workstreams code against these seams. A change to one goes to the lead first (message the lead, or put it
in its own commit on your branch and say so when you report). Headers named here are the source of truth;
this file says what they promise.

- `src/runtime/*.h` were written by the lead: heap.h, value.h, result.h, string.h, symbol.h, bigint.h,
  property.h, object.h, function.h, realm.h, vm.h, errors.h, operations.h, builtins.h. The runtime
  workstream implements them and may make additive changes (new declarations, new members); renames,
  removals and signature changes go through the lead. Other workstreams add nothing there.
- `src/parser/ast.h` and `src/parser/parser.h` follow the same rule with the parser workstream.

Conventions for every workstream: namespace `snowman` (sub-namespaces per area), C++20, `#include` paths
relative to `src/` (`#include "runtime/value.h"`), no exceptions for JS control flow, `make` warning-free.

## 1. CLI contract

- The contract is t262/README.md. `src/main.cpp` (lead) parses flags and dispatches through `src/engine.h`:
  `parseOnlyMain` and `dumpAstMain` (parser, `src/parser/cli.cpp`), `runMain` (interp, `src/interp/cli.cpp`).
- Errors go to stdout as `Name: message` (`SyntaxError: ...`, `TypeError: ...`, `Test262Error: ...`), exit 1.
  An uncaught non-Error value prints `Uncaught: <ToString(value)>` style text but still exits 1.
- Results format: the JSONL t262/runner/run.mjs writes and the JSON aggregate.mjs writes.

## 2. AST (`src/parser/ast.h`, owned by the parser workstream)

- **ESTree-shaped**: node types and field names as ESTree (es2025, as acorn `ecmaVersion: "latest"` produces
  them): `Program`, `ExpressionStatement` (+ `directive`), `FunctionDeclaration`, `ClassBody`,
  `MethodDefinition`, `PropertyDefinition`, `StaticBlock`, `PrivateIdentifier`, `ChainExpression`,
  `ImportExpression`, `MetaProperty`, `TemplateElement` (cooked may be absent), `Literal` (string, number,
  bigint, regex, boolean, null), `ImportAttribute`, … One C++ struct per node type, all deriving from
  `Node { NodeType type; uint32_t start, end; /* UTF-16 offsets */ uint32_t line, column; }`.
- **Lifetime**: arena-allocated, never freed; nodes outlive every function created from them.
- **Strings**: identifier names and cooked string values are `std::u16string` (escapes resolved; lone
  surrogates allowed). Number literals carry the `double`; BigInt literals the decimal digit string.
- **Source text**: the `Program` keeps the whole source (UTF-16) so `Function.prototype.toString` can slice
  `[start, end)` of a function, class or method node.
- **Static semantics**, computed by the parser and attached to nodes, named after the spec:
  - Program and every function: `strict`; `varNames` (VarDeclaredNames), `varScopedDeclarations`,
    `lexicallyScopedDeclarations` (each with its BoundNames and whether it is a constant declaration),
    `functionsToInitialize` (the hoisted function declarations, last one per name wins).
  - Functions: kind (normal, arrow, method, getter, setter, class constructor (base/derived), class field
    initializer), `isAsync`, `isGenerator`, `hasSimpleParameterList`, `parameterNames` (BoundNames),
    `hasParameterExpressions`, `argumentsObjectNeeded` (spec FunctionDeclarationInstantiation step 15–18),
    `hasDirectEval` (a direct `eval(...)` call in params or body, including nested arrows).
  - Blocks, `switch` case blocks, `for`/`for-in`/`for-of` heads, class bodies, catch clauses: their
    `lexicallyScopedDeclarations`.
  - Annex B.3.2/B.3.3: function declarations in blocks that are var-hoisting candidates (sloppy mode).
  - Modules: ImportEntries, LocalExportEntries, IndirectExportEntries, StarExportEntries, RequestedModules
    (with import attributes).
- The interpreter may cache data on nodes only through a field the parser provides for it
  (`mutable void* interpCache = nullptr;` on the nodes it needs, e.g. `Identifier`, function nodes).

## 3. Parser API (`src/parser/parser.h`, parser workstream)

- `ParseResult parseProgram(std::u16string source, const ParseOptions&)`: Script or Module goal; options
  carry the context direct eval needs (strict caller, inside a function (new.target), super property / super
  call allowed, inside a class field initializer (`arguments` forbidden), the private names in scope).
- `ParseResult parseDynamicFunction(kind, std::u16string params, std::u16string body)`: CreateDynamicFunction
  (Function, GeneratorFunction, AsyncFunction, AsyncGeneratorFunction): parse params and body separately,
  then together, with the spec's early errors.
- Errors: `ParseError { std::string message; uint32_t line, column; }`, always a SyntaxError.
- `std::u16string utf8ToUtf16(std::string_view)`.
- RegExp literals are validated at parse time through `snowman::regexp::validate` (section 10).

## 4. Values (`src/runtime/value.h`)

- `Value` is a tagged union: Undefined, Null, Boolean, Number (f64), String, Symbol, BigInt, Object, plus an
  internal Empty (holes, TDZ) that never escapes to JS. Construct with `Value::undefined()`,
  `Value::null()`, `Value::boolean(b)`, `Value::number(d)`, or implicitly from `String*`, `Symbol*`,
  `BigInt*`, `Object*` (non-null).
- `String`: immutable UTF-16 code units (`std::u16string_view view()`), lone surrogates allowed. Property-key
  strings are interned atoms (`atom(u"length")`), so key equality is pointer equality.
- `Symbol` (description: undefined or String), well-known symbols shared across realms (`vm().symbols`).
- `BigInt`: arbitrary precision, sign + magnitude.

## 5. Object model (`src/runtime/object.h`)

- `Object` has the 11 essential internal methods as virtuals named as in the spec (`GetPrototypeOf`,
  `SetPrototypeOf`, `IsExtensible`, `PreventExtensions`, `GetOwnProperty`, `DefineOwnProperty`,
  `HasProperty`, `Get`, `Set`, `Delete`, `OwnPropertyKeys`) plus `Call`/`Construct` (present iff
  `isCallable()`/`isConstructor()`). The base class implements the ordinary behaviour; exotic objects
  (Array, String, arguments, Proxy, TypedArray, module namespace, bound functions) override.
- `PropertyKey` is an array index (canonical numeric string ≤ 2³²−2), an atom, or a Symbol.
  `OwnPropertyKeys` order is the spec's: integer indices ascending, then strings, then symbols, each in
  creation order.
- `PropertyDescriptor` has optional fields (value, get, set, writable, enumerable, configurable).
- Internal slots: subclasses (`ArrayObject`, `ErrorObject`, `BooleanObject`, …) identified by
  `ObjectKind kind()`; downcast with `obj->as<T>()` / `obj->is<T>()`.

## 6. Abstract operations (`src/runtime/operations.h`)

- Free functions in `namespace snowman`, named and parameterised as in the spec: `ToPrimitive`,
  `ToBoolean`, `ToNumber`, `ToNumeric`, `ToString`, `ToObject`, `ToPropertyKey`, `ToIntegerOrInfinity`,
  `ToLength`, `ToIndex`, `ToInt32`, `ToUint32`, `Call`, `Construct`, `Get`, `GetV`, `Set`, `GetMethod`,
  `HasProperty`, `HasOwnProperty`, `CreateDataProperty(OrThrow)`, `DefinePropertyOrThrow`,
  `DeletePropertyOrThrow`, `Invoke`, `SameValue`, `SameValueZero`, `IsStrictlyEqual`, `IsLooselyEqual`,
  `IsCallable`, `IsConstructor`, `IsArray`, `LengthOfArrayLike`, `CreateArrayFromList`,
  `CreateListFromArrayLike`, `SpeciesConstructor`, `GetPrototypeFromConstructor`,
  `OrdinaryCreateFromConstructor`, `OrdinaryHasInstance`, `InstanceofOperator`, `GetIterator`,
  `IteratorStep`, `IteratorValue`, `IteratorClose`, `ToPropertyDescriptor`, `FromPropertyDescriptor`, …
  Add new ones with the spec's name in the header of the area that owns them.
- Throwing: `ThrowTypeError("msg")`, `ThrowRangeError`, `ThrowSyntaxError`, `ThrowReferenceError`, … create
  the error in the current realm and return a `Throw` (section 7).

## 7. Completion records (`src/runtime/result.h`)

- `Result<T>` is normal (a `T`) or throw (a `Value`). `Throw{value}` converts to any `Result<T>`.
- `TRY(expr)` evaluates to the normal value or returns the throw from the enclosing function (a GCC statement
  expression). `Result<void>` for operations with no value.
- Break/continue/return completions are the interpreter's own business (src/interp/).

## 8. Realm (`src/runtime/realm.h`)

- `Realm` holds the %Intrinsic% table (`realm->intrinsic(Intrinsic::ArrayPrototype)`; the `Intrinsic` enum is
  an X-macro list named after the spec's well-known intrinsics), the global object and the global
  environment (an interp-defined cell, opaque to the runtime).
- `CreateRealm()` builds a complete realm; several realms coexist (`$262.createRealm`). Built-ins always use
  the realm they were created in (a built-in function's [[Realm]]); `vm().currentRealm()` is the running
  execution context's realm.

## 9. Built-ins (`src/runtime/builtins.h`, one file per built-in in `src/builtins/`)

- A built-in file registers an installer with `SNOWMAN_BUILTIN(Name, order, installFn)`; `CreateRealm` runs
  installers by `order`, then name. Nothing central lists them, so adding a file touches no shared file.
- Helpers declare properties declaratively: `defineFunction(realm, target, key, length, nativeFn,
  attributes)`, `defineAccessor(...)`, `defineValue(...)`, `createBuiltinFunction(...)`. Default attributes
  are the spec's (writable, non-enumerable, configurable for methods; constructors' `prototype` non-writable,
  non-enumerable, non-configurable; `length`/`name` configurable only).
- Native signature: `Result<Value> fn(NativeCall& call)` with `call.thisValue`, `call.args` (`args[i]` is
  undefined past the end), `call.newTarget` (nullptr for [[Call]]), `call.callee`. Per-function state lives
  in the callee's internal slots (traced), never in captured C++ state.

## 10. RegExp (`src/regexp/regexp.h`, regexp workstream)

- `std::optional<std::string> validate(std::u16string_view pattern, std::u16string_view flags)`: early
  errors for literals and the RegExp constructor; nullopt = valid.
- The matcher API (compile once, exec at an index over UTF-16 input, capture spans, named groups) is added
  to the same header and described here before the RegExp built-in consumes it.

## 11. Unicode (`src/unicode/unicode.h`, regexp workstream after the lead's first version)

- `isIDStart`, `isIDContinue`, `isSpaceSeparator`; tables generated by `tools/unicode/` from node's Unicode
  17.0 data. Property tables for `\p{…}`, case folding and case mapping are added here.

## 12. Suspension, jobs, host

- Generators and async functions run on stackful coroutines (`src/interp/coroutine.h`): each has its own
  mmap'd stack, registered with the heap so the GC scans it conservatively. Await and yield are a switch back
  to the resumer.
- Jobs: `vm().enqueueJob(job)` (HostEnqueuePromiseJob); the CLI drains the queue after the script or module.
- Host: `print`, `$262` (`createRealm`, `evalScript`, `detachArrayBuffer`, `gc`, `global`, `agent` absent)
  live in src/interp/host.cpp.

## 13. Memory (`src/runtime/heap.h`)

- Every GC thing derives from `Cell` and is allocated with `make<T>(...)`. A cell that holds `Value`s or
  cell pointers must visit every one of them in `trace(Tracer&)`.
- Roots: the C stacks (main and coroutine stacks, scanned conservatively), realms, the job queue, and
  `ValueVector`/`Rooted` holders. A `std::vector<Value>` outside a cell is NOT a root: use `ValueVector`.
- First version allocates only; the collector is switched on later without changing these rules.
