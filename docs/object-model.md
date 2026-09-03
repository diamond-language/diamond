# Diamond object model

This document describes the implemented object model and its deliberate
metaprogramming boundaries.

## Current representation

Every managed allocation begins with `DiamondObject`, containing its kind,
mark bit, and allocation-list link.

An instance contains:

- a pointer to immutable `DiamondClass` metadata;
- a pointer to its current class-owned `DiamondShape`;
- a field count;
- a variable-sized array of `DiamondValue` fields.

Class metadata contains a name, optional superclass index, method table, and an
ordered instance-variable name table. Classes are module metadata, deliberately
not first-class Diamond objects -- see "Metaprogramming boundaries" below.

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

## Class variables

`@@cvar` values live on `DiamondVm`, not on the instance or on class
metadata: `DiamondClass` (module-owned, pointer-free, and cloned
byte-for-byte across a `Thread` boundary, see `docs/threads.md`) carries
only the compile-time *name*-to-slot table, mirroring the instance-field
name table above. The values themselves are a flat, lazily-`calloc`'d
`DiamondValue` array on the VM, indexed as `class_index * DIAMOND_MAX_FIELDS
+ slot`, allocated on first write and left `nullptr` (reads default to
`nil`, cheaply, with no allocation) for any VM that never touches one.

```text
GET_CVAR destination, class_index, slot
SET_CVAR class_index, slot, source
```

Both operands past the payload register are compile-time constants, not
registers -- resolved once per `@@name`, the same way a namespace
constant's index is, and requiring no receiver: `def self.x` methods have
no bound `self` in this VM at all (see "Methods and calls" below), so
`class_index` alone -- not a runtime object -- is what identifies which
slot table applies. This is also why the storage is per-`DiamondVm`
rather than embedded in `DiamondClass` directly: a `DiamondValue` can hold
a live GC object pointer, and `DiamondClass`'s own byte-for-byte
`Thread.new` clone would silently carry that pointer into a different
heap. The lazy allocation isn't just an optimization, either -- embedding
the full fixed-size grid inline in `DiamondVm` measurably shrank the
safety margin `DIAMOND_MAX_CALL_DEPTH`'s stack-depth guard depends on, since
`DiamondVm` is stack-allocated once underneath every recursive `run_chunk`
call chain (`src/run_source.c`, `src/repl.c`); a pointer that's usually
`nullptr` costs nothing there.

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
Class variables are VM-owned roots, not reachable through any instance or
class pointer -- `diamond_vm_collect` traces the whole flat array directly
(skipped entirely when never allocated).

## Metaprogramming boundaries

Runtime shapes, polymorphic method inline caches, and field caches keyed by
shape identity (above) are all implemented, not planned. Modules/mixins,
singleton classes, and visibility are implemented too (see `docs/design.md`).
Runtime method *redefinition* — repointing an existing method to a different
already-compiled function via `ClassName.redefine_method(name, callable)` —
is also implemented. Defining genuinely new method bodies at runtime (e.g.
from a source string) remains unimplemented and would require a
runtime-callable compiler entry point, a materially larger undertaking
overlapping with self-hosting.

Classes becoming ordinary instances of `Class` is not planned — decided
against; see `docs/roadmap.md`'s "Explicitly deferred" section for why,
given the representation described above.

Those optimizations should preserve the current receiver convention and source
semantics, but they are not represented in the current bytecode format.
