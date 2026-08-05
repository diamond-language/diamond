# Diamond object model

This document distinguishes the implemented object model from planned runtime
optimizations.

## Current representation

Every managed allocation begins with `DiamondObject`, containing its kind,
mark bit, and allocation-list link.

An instance contains:

- a pointer to immutable `DiamondClass` metadata;
- a field count;
- a variable-sized array of `DiamondValue` fields.

Class metadata contains a name, optional superclass index, method table, and an
ordered instance-variable name table. Classes are module metadata, not currently
first-class Diamond objects.

## Fields

The compiler assigns a stable offset whenever a method first mentions an
instance variable. A subclass begins with a copy of its parent's field table,
preserving inherited offsets. Instance allocation reserves every field known to
the class and initializes it to `nil`.

Current bytecode uses numeric offsets directly:

```text
GET_IVAR destination, receiver, field_offset
SET_IVAR receiver, field_offset, source
```

This is a fixed class layout, not runtime hidden-class/shape transitions.

## Methods and calls

A method is an ordinary bytecode function whose register zero contains `self`.
Explicit parameters begin in register one.

```text
INVOKE destination, receiver, method_name, argument_base, argument_count
```

Lookup is currently a linear search through the receiver's class method table,
then each superclass. There is no inline cache yet.

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
- Modules/mixins, singleton classes, visibility, and runtime method definition.

Those optimizations should preserve the current receiver convention and source
semantics, but they are not represented in the current bytecode format.
