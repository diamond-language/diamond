# Diamond object model

This document distinguishes the implemented object model from planned runtime
optimizations.

## Current representation

Every managed allocation begins with `DiamondObject`, containing its kind,
mark bit, and allocation-list link.

An instance contains:

- a pointer to immutable `DiamondClass` metadata;
- a pointer to its current class-owned `DiamondShape`;
- a field count;
- a variable-sized array of `DiamondValue` fields.

Class metadata contains a name, optional superclass index, method table, and an
ordered instance-variable name table. Classes are module metadata, not currently
first-class Diamond objects.

## Fields

The compiler assigns a stable offset whenever a method first mentions an
instance variable. A subclass begins with a copy of its parent's field table,
preserving inherited offsets. Instance allocation reserves every field known to
the class, but starts at shape zero and lazily advances its materialized prefix
on writes. Reads beyond that prefix return `nil`.

Current bytecode uses numeric offsets directly:

```text
GET_IVAR destination, receiver, field_offset
SET_IVAR receiver, field_offset, source
```

Offsets remain fixed while shape pointers make field materialization explicit.

Class methods retain those numeric instructions. Reusable module methods emit
`GET_IVAR_NAME` and `SET_IVAR_NAME` with a function-local field-name constant.
Including the module merges its field names into the receiving class metadata.
At execution, the symbolic instruction resolves the offset from the actual
receiver class, then enters the same shape-transition and polymorphic-cache path
as an ordinary numeric field instruction. Transitive includes carry field
requirements, and equal names deliberately resolve to the same class slot.

## Methods and calls

A method is an ordinary bytecode function whose register zero contains `self`.
Explicit parameters begin in register one.

```text
INVOKE destination, receiver, method_name, argument_base, argument_count
```

Lookup misses search the receiver's class method table and then each superclass.
Dynamic invoke sites retain up to four class/method resolutions in VM-owned
polymorphic inline caches.

`ClassName.new(arguments)` allocates an instance and looks up `initialize`. Its
return value is ignored; `new` returns the instance. Inherited constructors work
through the same lookup chain.

`super(arguments)` records the lexical owner class and method name in bytecode.
Runtime lookup starts at that owner's superclass while preserving the original
receiver, avoiding recursion into a subclass override.

## Collection semantics

Instances trace every field. Their class pointers refer to module-owned metadata
whose lifetime exceeds VM execution, so class metadata is not marked. Arrays and
hashes can form arbitrary nested object graphs and are recursively traced.

## Planned evolution

- Runtime shapes for objects whose fields evolve dynamically.
- Monomorphic, then potentially polymorphic, method inline caches.
- Field caches keyed by shape identity.
- Classes as ordinary instances of `Class`.
- Modules/mixins, singleton classes, and visibility are implemented (see
  `docs/design.md`). Runtime method *redefinition* — repointing an existing
  method to a different already-compiled function via
  `ClassName.redefine_method(name, callable)` — is also implemented. Defining
  genuinely new method bodies at runtime (e.g. from a source string) remains
  unimplemented and would require a runtime-callable compiler entry point,
  a materially larger undertaking overlapping with self-hosting.

Those optimizations should preserve the current receiver convention and source
semantics, but they are not represented in the current bytecode format.
