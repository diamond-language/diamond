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

For maintainability, the prelude is being assembled from logical source modules
at the native embedding boundary while `lib/core.di` remains the compatibility
entry point. `lib/core/string_builder.di` and `lib/core/json.di` are the first
extracted modules; the CLI and REPL append them to the embedded core source in
dependency order before compilation.

Investigate in this order:

1. split the prelude into coherent source modules, beginning with JSON;
2. measure conservative source-driven module selection for optional facilities;
3. design a reusable compiled-prelude snapshot or compiler append mode if source
   selection cannot deliver a meaningful improvement;
4. preserve CLI/test semantic parity and source-mapped diagnostics throughout.

A physical file split alone is not a performance improvement: ordinary
`require` expansion still flattens and recompiles every selected file.

### Continue the Arel relational algebra

The Arel package now has expression nodes, grouping, joins, correlated
subqueries, CTEs, set operations, and write statement managers, rendered
through three dialect visitors -- `Arel::SQLiteVisitor` (the original,
still the default), `Arel::PostgreSQLVisitor`, and `Arel::MariaDBVisitor`
(named for the server it was actually verified against, since some of
what it supports, like `RETURNING`, is MariaDB-specific rather than true
of MySQL generally). Each was verified against a live server, not
assumed from similarly-named syntax; the four items this section used to
call "deferred expression decisions" (per-column `DEFAULT`,
named-constraint conflict targets, parameterized `CAST` types, additional
operators) are resolved. `diamond-active_record` (`packages/
diamond-active_record/`) now sits on top of Arel as an explicit,
low-magic persistence layer (`Repository`, four association kinds,
optimistic locking, eager loading, batch iteration, nested transactions
via savepoints), plus an optional `ActiveRecord::Model` layer for a more
Rails-familiar surface. Remaining forward plan lives in
[../packages/arel/ROADMAP.md](../packages/arel/ROADMAP.md): a fourth
dialect (real work now, since MariaDB was the one with an existing
native driver to build on) and the still-deferred "additional operators"
item.

### Improve receiver-aware language tooling

The LSP now resolves `receiver.method` (completion, hover, definition) for the
statically-known-without-real-type-inference receiver forms: a literal class
name, `self` inside an instance method or a class-owned `def self.x`, a local
variable last known (at its own declaration) to hold `ClassName.new(...)`
(`DiamondScopeLocal.known_type`, `src/vm.h`, a byte snapshot of the compiler's
existing per-register `known_types` state, taken at the exact point that
local's scope closes), and now also a union receiver -- a parameter (or a
local initialized from one) given an explicit `pet: Dog | Cat` annotation
(`DiamondScopeLocal.known_type_set`, the same snapshot idea applied to
`known_type_sets`, decoded against the owning function's own `type_sets[]`).
Resolves against every class-kind union member that defines the method: one
signature when they agree, `ClassName#signature` per match when they don't,
a `Location[]` from go-to-definition when there's more than one match. Only
new compiler state this needed, either time. Confirmed directly (not
assumed) that the compiler does **not** build a union type from a branching
assignment like `x = cond ? Dog.new() : Cat.new()` -- `parse_if`'s merge only
keeps a type-set match when both branches already agree on the exact same
set index (`src/compiler.c`), so that shape still isn't resolvable here; the
only real source of a multi-class union is an explicit source-level
annotation. See `docs/lsp.md` and `lsp/receiver.c` for the mechanism and its
scope cuts in full.

Remaining, still open:

- an instance-variable receiver, a chained call's return value as a receiver,
  a local reassigned to a different class later in the same scope
  (`known_type`/`known_type_set` reflect first declaration, not a later
  reassignment), or a branch-merged "union" the compiler doesn't actually
  track as one;
- dependency-aware symbol information beyond one combined compilation;
- incremental compilation only after there is a compiler architecture that can
  benefit from incremental document synchronization.

## Self-hosting: minimal-compat maintenance mode

The Diamond compiler can compile and run itself (self-parse and self-run
bootstrap, `tests/self_host_smoke.sh`), but growing full parity was premature:
the native language itself isn't stable enough yet for keeping a second,
hand-ported frontend in lockstep to be worth its ongoing cost. Self-hosting
work is paused here, not abandoned -- revisit once the native surface (syntax,
diagnostics, opcode set) has settled enough that parity effort mostly stays
spent rather than being repeatedly re-paid.

While paused:

- `make test-all` runs only the two bootstrap smoke checks (self-parse,
  self-run) -- enough to know the self-hosted frontend hasn't gone
  completely stale, not full parity coverage;
- the exhaustive differential corpus (`tests/lexer_diff.sh`,
  `tests/parser_diff.sh`, together `make test-self-host`, ~1400 cases plus
  one-off scenarios) is opt-in/periodic rather than run on every push -- it
  used to dominate `make test-all`'s wall time (as much as ~27 of ~43
  minutes on CI) for a reason unrelated to test-harness inefficiency: the
  self-hosted parser's own per-case cost is dominated by re-parsing all of
  `lib/core.di` through the interpreter every time (confirmed by profiling
  `ProgramBuilder#run` directly -- verify+execute there is ~1ms; the cost is
  entirely in `parser.compile()` itself), which is inherent to running an
  interpreter-implemented parser one VM level deep, not something a batching
  fix resolves;
- known parity gaps (class-variable syntax and semantics still missing from
  parts of the self-hosted parser, hand-maintained opcode-number mirrors
  instead of one generated source of truth, newer native syntax/diagnostics
  the self-hosted side hasn't picked up) are left as known gaps rather than
  active work;
- the internal `ProgramBuilder` API remains explicitly unstable, as before.

Resuming this work later should start by re-measuring whether re-parsing
`lib/core.di` per case is still the dominant cost, and whether the self-hosted
parser can parse it once and reuse that state across cases instead of from
scratch every time.

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
instances of `Class`/`Module` in the general sense -- there is still no way to
pass a class as an ordinary argument, store one as an attribute, or name one
dynamically by a computed string, and a fully reified metaobject model (every
class a real, GC-owned heap instance, first-class the way Ruby's `Class` is)
remains undone. It would affect dispatch, GC ownership, constants,
self-hosting, and cross-program values, and should stay motivated by a
concrete capability rather than Ruby resemblance -- which is exactly what
happened for one narrow slice of it: `self` inside a class-owned `def self.x`
method now evaluates to a lightweight `DIAMOND_VALUE_CLASS` value (a 1-byte
class index carried in `DiamondValue`'s existing union, no heap allocation,
no GC changes), and `self.foo(...)` there dispatches virtually against it --
built specifically so `ActiveRecord::Model` could provide shared, inherited
class-level methods (`self.find`/`.all`/`.where`/`.create`/`.find_each`/
`.find_in_batches`) instead of requiring each subclass to redeclare them. See
`docs/design.md` and `docs/syntax.md` for the full mechanism and its
deliberately narrow scope. General reification -- classes as fully ordinary,
freely-passable runtime values -- is still the larger, undone question this
section originally posed.

### Stable compiler boundary

`ProgramBuilder` exists to support the self-hosted bootstrap. It is not a stable
embedding API. A public compiler API would need explicit ownership, source-map,
versioning, and cross-program type semantics rather than exposing the current
internal structure by accident.
