# Interfaces (owned by the project lead; change only through them)

Fill these in before staffing:

1. **CLI contract:** see README. Results format: the kit's JSONL and JSON.
2. **AST:** ESTree-shaped nodes with source spans, plus `--dump-ast` JSON to diff against acorn.
3. **Values:** UTF-16 strings that may hold lone surrogates, f64 Number, BigInt, Symbol, Object.
4. **Object model:** the 11 essential internal methods plus [[Call]] and [[Construct]], so exotic objects
   (Array, Proxy, TypedArray, arguments, String, module namespace) plug in. Own-key order and array-index keys
   follow the spec.
5. **Abstract operations:** named as in the spec (ToPrimitive, Call, SpeciesConstructor, …).
6. **Completion records:** an explicit result type, normal or throw.
7. **Realm:** a %Intrinsic% table, with several realms supported (`$262.createRealm`).
8. **Built-ins:** registered declaratively (name, length, attributes), one file per built-in.
9. **Suspension:** how generators and async functions suspend (bytecode VM or explicit frames), the job queue,
   and the host hooks.
