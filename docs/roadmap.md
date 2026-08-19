# Diamond roadmap

This document contains future directions and open decisions. Completed work
belongs in [../CHANGELOG.md](../CHANGELOG.md); detailed behavior belongs in the
topic documents under `docs/`.

Diamond is a research language, so priorities may change when measurement or
implementation work reveals a more valuable question.

## Near-term direction

### Reduce prelude compilation cost

`lib/core.di` is prepended and compiled from source for every program. The batch
test runner reuses its large `DiamondProgram`, but still recompiles the complete
prelude for every case. Recent measurements show the batch corpus is now
CPU-bound enough for that repeated work to matter.

Investigate in this order:

1. split the prelude into coherent source modules, beginning with JSON;
2. measure conservative source-driven module selection for optional facilities;
3. design a reusable compiled-prelude snapshot or compiler append mode if source
   selection cannot deliver a meaningful improvement;
4. preserve CLI/test semantic parity and source-mapped diagnostics throughout.

A physical file split alone is not a performance improvement: ordinary
`require` expansion still flattens and recompiles every selected file.

### Continue the Arel relational algebra

The SQLite-first Arel package now has expression nodes, grouping, joins, and
subqueries. Its own forward plan lives in
[../packages/arel/ROADMAP.md](../packages/arel/ROADMAP.md). The next architectural
steps are correlated subqueries, CTEs, and set operations, followed by write
statement managers.

### Close self-hosted frontend parity gaps

The Diamond compiler can compile and run itself, but parity work remains
open-ended. Known or likely targets include:

- class-variable syntax and semantics still missing from parts of the
  self-hosted parser;
- differential sweeps for newer syntax and diagnostics;
- eliminating hand-maintained opcode-number mirrors or generating them from one
  source of truth;
- making the self-hosted frontend usable as more than an internal bootstrap
  experiment without declaring its internal `ProgramBuilder` API stable.

### Improve receiver-aware language tooling

The LSP understands declarations and locals recorded by a compiled program, but
method completion/hover/definition through `receiver.method` requires a useful
receiver type and potentially several candidate classes.

Potential independent slices:

- receiver-aware method completion for statically known nominal types;
- hover and definition for the same constrained case;
- union receiver results with explicit ambiguity handling;
- dependency-aware symbol information beyond one combined compilation;
- incremental compilation only after there is a compiler architecture that can
  benefit from incremental document synchronization.

## Runtime research

### Bound garbage-collection pauses

The collector is non-moving, stop-the-world mark/sweep. `bench/gc_churn`
establishes that individual pause duration grows with the persistent live set,
even though aggregate GC CPU share does not run away in the measured workloads.

A first generational implementation was built, stress/sanitizer tested, measured,
and reverted. Its remembered-set and promotion machinery added complexity but
did not improve the target workload enough to justify keeping it. The design
notes and failure analysis are retained in
[gc-generational-design.md](gc-generational-design.md).

Any next attempt should begin with a revised invariant and benchmark target,
not simply reapply the reverted design. Plausible directions include:

- cheaper remembered-set maintenance with an explicitly proven major-GC
  invariant;
- incremental marking to bound pauses without a nursery;
- arena or region allocation for compiler-lifetime objects;
- reducing allocation volume in core-library hot paths before changing the
  collector.

### Native-code execution

Nothing currently generates native code. Existing work optimizes the interpreter
through register allocation, shapes, caches, monomorphic rewrites, and optional
opcode quickening.

A JIT or native backend should be attempted only around a measured workload and
should begin with one narrow compilation tier. The existing
`jit-experimentation` branch records interpreter experiments that did not justify
shipping additional specialization complexity.

### Compiler representation

Diamond deliberately has no retained AST and uses monotonically allocated
registers within a function. This keeps the compiler understandable, but limits
some future work.

Open questions:

- whether a small intermediate representation is warranted for optimization,
  reusable prelude compilation, or native code generation;
- whether register reuse can reduce large-function bytecode/frame pressure
  without obscuring source facts and closure capture;
- whether forward declarations are worth a predeclaration pass, given the
  current single-pass compiler's simplicity;
- which fixed table/offset limits should be widened, removed, or kept as
  deliberate implementation boundaries;
- replace the interim embedded 1024-entry function table with dynamically
  allocated function storage before approaching the 16-bit bytecode index
  boundary, without invalidating compiler or VM function references.

## Language and library directions

### Ruby-like ergonomics without Ruby compatibility

Continue adding familiar constructs only when they compose naturally with
Diamond's object model. Candidates should be evaluated as independent slices,
with explicit semantics rather than assumed Ruby parity.

Areas still worth examining include:

- richer pattern matching beyond equality-based `case`/`when`;
- indexed compound assignment and range-based collection slicing;
- protected visibility, if a real library design needs it;
- enumerator/lazy iteration semantics versus the current eager collection APIs;
- a principled protocol for native collection extension instead of expanding
  VM name-forwarding tables indefinitely.

### Explicit-arity method delegation

Add a deliberately scoped first version of class/module delegation with the
target and complete parameter list visible in source:

```ruby
class Account
  delegate owner_name(), to: @owner
  delegate charge(amount), to: @billing
end
```

Initially restrict targets to instance variables and compile each declaration
into an ordinary forwarding method. That preserves Diamond's existing method
metadata, visibility, inheritance, interface checks, dispatch caches, arity
validation, and `respond_to?` behavior instead of adding a parallel runtime
dispatch model. The native and self-hosted parsers must remain in parity, and
generated methods should appear in language tooling like any other method.

Do not infer arity from an untyped target or add Rails-style name-only
delegation in this slice. Arbitrary forwarding depends on variadic/splat call
support Diamond does not currently have; `delegate_missing_to` additionally
depends on a general missing-method protocol. Both remain separate future
design questions.

### Native service depth

The current native APIs intentionally expose useful, narrow slices. Possible
extensions should be demand-driven:

- prepared SQLite statements, transaction helpers, named binds, and open flags;
- asynchronous subprocess handles with polling and termination;
- TLS ALPN, client certificates, session resumption, and custom trust stores;
- richer time parsing/timezone support;
- surfacing stdout write failures as rescuable exceptions.

### Package ecosystem

`facet` uses git URLs and exact refs. A hosted registry, semantic-version
selection, and multi-version installation are not implied next steps: Diamond's
single flat compiled namespace cannot currently host two versions of the same
package safely.

Smaller viable improvements include:

- `facet init` and manifest-editing commands;
- clearer dependency-conflict explanations;
- package-level focused test conventions;
- extracting repository packages into independent remotes when they acquire
  real external consumers.

## Explicitly deferred

- Ruby compatibility as a goal;
- stable bytecode or embedding APIs;
- multi-platform portability work;
- a hosted package registry without a package-identity/version model;
- shared-heap threads;
- a runtime `eval`/source compiler solely to emulate Ruby metaprogramming.

## Open design decisions

### Forward and mutual calls

Bare calls resolve only previously declared top-level functions in file order.
Receiver-based method calls resolve dynamically and are the existing workaround
for mutually recursive methods. Fixing bare forward calls requires at least a
declaration-discovery pass and must be weighed against the intentionally direct
compiler.

### Classes as ordinary runtime objects

Class and module metadata remain program-owned structures rather than ordinary
instances of `Class`/`Module`. Moving to a fully reified metaobject model would
affect dispatch, GC ownership, constants, self-hosting, and cross-program values;
it should be motivated by a concrete capability rather than Ruby resemblance.

### Stable compiler boundary

`ProgramBuilder` exists to support the self-hosted bootstrap. It is not a stable
embedding API. A public compiler API would need explicit ownership, source-map,
versioning, and cross-program type semantics rather than exposing the current
internal structure by accident.
