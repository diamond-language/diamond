# Diamond roadmap

This document contains future directions and open decisions. Completed work
belongs in [../CHANGELOG.md](../CHANGELOG.md); detailed behavior belongs in the
topic documents under `docs/`.

Diamond is a research language, so priorities may change when measurement or
implementation work reveals a more valuable question.

## Near-term direction

### Reduce prelude compilation cost

`lib/core.di` (780 lines) is prepended and compiled from source for every
program, alongside four extracted modules -- `lib/core/string_builder.di`,
`lib/core/numeric.di`, `lib/core/json_codec.di`, `lib/core/json.di` -- all
five assembled by one shared helper, `src/prelude.c`/`.h`
(`diamond_prelude_needs_json`/`_length`/`_write`), which every embedding
site (`src/run_source.c`, `src/repl.c`, `lsp/compile_buffer.c`,
`lsp/diagnostics.c`) now calls into instead of each keeping its own copy --
that duplication had drifted once already (`lsp/compile_buffer.c`/
`lsp/diagnostics.c` fell behind to embedding `lib/core.di` alone, silently
breaking LSP support for any document using JSON/StringBuilder/numeric
functions; fixed, then consolidated so it can't drift a second time).

Steps 1 and 2 of this section's own investigation order are done, with
measured numbers rather than estimates (a standalone harness timing
`diamond_compile()` directly): `lib/core.di` alone compiles in ~3.85ms,
the full 5-file prelude in ~6.53ms -- `json_codec.di`+`json.di` account for
~1.78ms of that (~27%), while `string_builder.di`+`numeric.di` together
cost under 1ms and were left always-included (too little to gain, and
`numeric.di`'s bare function names like `abs`/`min`/`max` are far more
likely to false-positive/negative under a text search than JSON's
distinctive `JSON.`/`JSONCodec`/`JSONError`). `diamond_prelude_needs_json`
does exactly that conservative substring search -- a superset match by
design, since a false "skip" breaks compilation while a false "include"
only costs the ~1.78ms being saved -- over the exact text that will
compile (the raw document, or a `require`-resolved bundle where one
exists), and `json_codec.di`/`json.di` are skipped whenever it finds
nothing. Measured corpus effect: `./build/run_cases tests/cases
build/case_output` (1,072 cases, one process, release build) went from
3.744s to ~3.18s real (repeatable across several runs) -- a real ~15%
reduction, smaller than a naive per-compile-percentage estimate would
suggest since many cases' own user code, not the prelude, dominates their
individual compile cost.

Step 3 (a reusable compiled-prelude snapshot or compiler append mode) is
deliberately not pursued now -- source selection already delivered a real,
measured win without it, matching this section's own stated condition for
skipping straight to step 3 ("if source selection cannot deliver a
meaningful improvement"). Revisit only if a future measurement shows the
remaining always-included prelude cost (`core.di`+`string_builder.di`+
`numeric.di`, ~4.75ms) still dominates in a way source selection alone
can't address.

### Continue the Arel relational algebra

The Arel package now has expression nodes, grouping, joins, correlated
subqueries, CTEs, set operations, and write statement managers, rendered
through four dialect visitors -- `Arel::SQLiteVisitor` (the original,
still the default), `Arel::PostgreSQLVisitor`, `Arel::MariaDBVisitor`
(named for the server it was actually verified against, since some of
what it supports, like `RETURNING`, is MariaDB-specific rather than true
of MySQL generally), and now `Arel::MySQLVisitor`, real MySQL 8. Each was
verified against a live server, not assumed from similarly-named syntax;
the four items this section used to call "deferred expression decisions"
(per-column `DEFAULT`, named-constraint conflict targets, parameterized
`CAST` types, additional operators) are resolved. Adding the fourth
dialect corrected an assumption this section used to make: it turned out
not to need new native connectivity at all -- Diamond's "MariaDB" native
support was never MariaDB-branded at the native layer, it's a `MySQL`
class already speaking the real MySQL wire protocol
(`packages/arel/ROADMAP.md` has the full comparison, including the one
real MySQL-specific quirk found: its row-alias upsert syntax has no
`INSERT ... SELECT` equivalent, unlike its `VALUES(...)`-list form).
`active_record` (`packages/active_record/`) now sits on
top of Arel as an explicit, low-magic persistence layer (`Repository`,
four association kinds, optimistic locking, eager loading, batch
iteration, nested transactions via savepoints), plus an optional
`ActiveRecord::Model` layer for a more Rails-familiar surface. The
"additional operators" item is now closed, deliberately not pursued --
checked its full history and it was explicitly left open twice before
for the same reason ("no concrete need has surfaced yet"), still true;
see [../packages/arel/ROADMAP.md](../packages/arel/ROADMAP.md) for the
full reasoning. Remaining forward plan there is just a fifth dialect
whenever one is worth adding.

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
- which fixed table/offset limits should be widened, removed, or kept as
  deliberate implementation boundaries;

Resolved: a class/module/interface (and a type annotation naming one) can
now be referenced before its own declaration is textually reached later in
the same source -- `diamond_compile` (`src/compiler.c`) runs the whole
source twice: a throwaway first pass (`discovery_pass` on `Compiler`)
tolerates an unresolved forward reference just long enough to walk the
entire file and fully register every class/module/interface's name,
fields, and methods (including singleton methods) regardless of order,
then a real second pass runs with everything already known, resuming
work on each pre-registered slot (`declared_by_discovery` on
`DiamondClass`/`DiamondModule`/`DiamondInterface`) instead of erroring on
a rediscovered name. See `tests/cases/forward_declarations.di`. This is
*not* a general predeclaration/IR change -- there is still no retained
AST, and a superclass/base-interface still needs to be declared first
(that copies the referenced declaration's already-*fully-compiled* field
table, not just its name -- seeing it registered isn't enough, see
`tests/cases/forward_declaration_superclass_still_fails.di`). Top-level
bare function forward/mutual calls are a related but separate, still-open
case -- see "Forward and mutual calls" below.

Resolved separately (built on the same `declared_by_discovery` machinery,
but a genuinely different feature): a `class`/`module` can now be
**reopened** -- a second `class Name`/`module Name ... end` for a name
that already exists adds methods/fields/nested classes to it instead of
erroring, whether the second declaration is later in the same file or (the
actual motivation) in a separate file pulled in by `require`. This is what
makes a real per-class-per-file split of a large package like
`packages/arel/lib/arel.di` possible without a workaround -- that package
wraps its whole class list in one `module Arel ... end`, and every class
file splitting it needs to reopen that same module. `declared_by_discovery`
now means "populated by a *different, already-finished* compile pass, not
yet touched by the one currently running" -- exactly one reset happens the
first time the *current* pass touches such a slot, and every subsequent
sighting within that same pass (a genuine reopen) merges without
resetting. Redefining an existing method name via reopen is still a
compile error (`compile_definition`'s existing duplicate-method check
needed no changes at all to keep enforcing this); a reopen's own `<
Super` clause is validated against whatever superclass the class already
has rather than re-applied, erroring (`superclass mismatch for reopened
class`) on a genuine conflict rather than silently changing what already-
written code inherits from. Interfaces don't support reopening -- kept
out deliberately, narrower in scope than what was asked for. See
`tests/cases/module_and_class_reopening.di` and
`tests/multifile/reopen_main.di` (the cross-file case, combined with a
forward reference, in one test).

## Language and library directions

### Ruby-like ergonomics without Ruby compatibility

Continue adding familiar constructs only when they compose naturally with
Diamond's object model. Candidates should be evaluated as independent slices,
with explicit semantics rather than assumed Ruby parity.

**Done**: indexed compound assignment (`arr[i] += 1`, `h[k] -= 1`, ...,
`src/compiler.c`'s `compile_index_compound_assignment`, alongside
`compile_index_assignment`/`compile_compound_assignment`) -- pure
compiler-level sugar over the existing `DIAMOND_OP_INDEX_GET`/`SET`
opcodes and `compile_binary_op`, no VM or opcode changes. The index
expression is evaluated exactly once (the register `parse_expression`
already returns for it is reused for both the read and the write back),
matching how the receiver register was already reused for both halves of
a plain `arr[i] = v`. Confirmed directly against every receiver shape
ordinary indexed assignment supports (local, `@ivar`, `@@cvar`, a
captured/boxed local) and all seven compound operators, including
`||=`/`&&=`'s real short-circuit semantics (the right-hand side isn't
evaluated at all when short-circuited) -- see
`tests/cases/indexed_compound_assignment.di`.

Areas still worth examining include:

- richer pattern matching beyond equality-based `case`/`when`;
- **Done**: range-based Array slicing (`arr[1..3]` read,
  `arr[1..3] = [...]` write). `Range` has no native VM value kind at
  all -- it's a plain user-space class (`lib/core.di`) -- so
  `DIAMOND_OP_INDEX_GET`/`SET` recognize one by comparing an index
  operand's own `->class` against a `Range` class index resolved once
  by name at the end of `diamond_compile` and cached on `DiamondVm`
  (confirmed directly that `DiamondChunk`, not `DiamondVm`, was the
  wrong place for this: a nested function/closure call builds its own
  fresh `DiamondChunk` view at the call site, none of which propagate a
  program-wide field like this one, so the first version of this cache
  silently stopped working inside any nested `def` -- caught before
  shipping). Slice write requires the replacement's length to exactly
  match the range's own (already-clamped) length -- no Ruby-style
  grow/shrink splice in this version, raising `TypeError` and leaving
  the array untouched otherwise. `Hash` is untouched (real Ruby doesn't
  support Range-based `Hash#[]` either). See `docs/syntax.md` for the
  full bounds/clamping rules;
- protected visibility, if a real library design needs it;
- enumerator/lazy iteration semantics versus the current eager collection APIs;
- a principled protocol for native collection extension instead of expanding
  VM name-forwarding tables indefinitely.

### Explicit-arity method delegation

**Done**: a deliberately scoped first version of class/module delegation,
with the target and complete parameter list visible in source:

```ruby
class Account
  delegate owner_name(), to: @owner
  delegate charge(amount), to: @billing
end
```

Targets are restricted to instance variables (`@name`, both classes and
modules -- a module's own field goes through the same name-keyed
`GET_IVAR_NAME` path `attr_reader` already uses for module state), and
each declaration compiles into an ordinary forwarding method (`src/
compiler.c`'s `compile_delegate`, alongside `compile_attribute`/
`compile_alias_method`) -- as if the source had literally been
`def name(params) @ivar.name(params) end`. Confirmed directly: the
generated method participates in inheritance/override/`super`,
`respond_to?`, and `redefine_method`/method-cache invalidation exactly
like a hand-written one would (`tests/cases/method_delegation.di`),
since it *is* one -- not a parallel dispatch model. Parameters are bare
names only (no type annotations, no defaults, no splat/block
forwarding); the forwarded call always uses the same name declared
(`delegate foo(), to: @bar` always calls `@bar.foo()`, never a renamed
target) -- both deliberate scope cuts, not oversights.

**Native-only, not self-hosted**: the self-hosted parser
(`selfhost/lexer.di`/`parser.di`) does not recognize `delegate` --
self-hosting is paused elsewhere in this document (growing new feature
parity isn't happening right now, only the two bootstrap smoke checks
are kept green), and nothing in the self-hosted parser's own source uses
`delegate`, so this is a deliberate, documented parity gap rather than a
silent one. Revisit if self-hosting work ever resumes in earnest.

Do not infer arity from an untyped target or add Rails-style name-only
delegation in this slice. Arbitrary forwarding depends on variadic/splat call
support Diamond does not currently have; `delegate_missing_to` additionally
depends on a general missing-method protocol. Both remain separate future
design questions.

### Runtime method synthesis

**Done, in scope**: `ClassName.compile_method(name, params, body_source,
bound_values)` compiles a method body from a source string at runtime and
returns a `Callable` for the existing `define_method` to install --
closing the specific gap `active_record`'s own docs have called out
repeatedly ("no `has_many :books`-style macro... blocked on a general
metaprogramming/macro system"). See `docs/design.md`'s "Runtime method
synthesis" section for the full mechanism (a throwaway, permanently-
adopted satellite program per call, field-count-validated against the
target class, `bound_values` for referencing values from classes the
synthesized source has no way to name directly) and
`packages/active_record/README.md`'s worked `has_many` example built on
it. Confirmed directly, not assumed: passed the full test corpus and
rack's own suite under `DIAMOND_STRESS_GC=1` (GC on every allocation)
before treating this as safe, since it's the first feature that made
`DiamondMethod`/`vm->adopted_programs` hold live, markable GC values at
all.

Two real architecture gaps surfaced and were fixed while building this,
not just for this feature's own sake: `DiamondVm.root_chunk` (instance
dispatch inside a compiled-method body now correctly falls back to the
receiver's real home chunk, not whichever chunk happens to be ambient --
previously those were always the same chunk, so nothing distinguished
them) and the same seeding applied to `Thread.new`'s own trampoline
(which calls `run_chunk` directly, bypassing `diamond_vm_run` where
`root_chunk` is normally set).

**Deliberately not in scope**: `self.` class-owned singleton methods
(instance methods only); closures over the *calling* scope's own locals
(only `bound_values`, fixed at `compile_method` time, and the target
class's existing fields are visible -- this is not a general `eval` and
was never meant to become one); bare parameter names only, matching
`delegate`'s own existing scope cut. `method_missing` is a separate,
smaller, already-scoped-out follow-up (a fallback at the existing
dispatch-miss branch, `lookup_method`/`lookup_method_cached` already
being the single choke point every call site shares) -- not attempted
here.

### Native service depth

The current native APIs intentionally expose useful, narrow slices. Possible
extensions should be demand-driven:

- prepared SQLite statements, transaction helpers, named binds, and open flags;
- asynchronous subprocess handles with polling and termination;
- TLS ALPN, client certificates, session resumption, and custom trust stores;
- richer time parsing/timezone support;
- surfacing stdout write failures as rescuable exceptions.

**Done**: password hashing and a CSPRNG, driven by a real need (building an
authentication system on Diamond) rather than speculatively. `BCrypt.hash`/
`.verify` (`src/vm.c`) is real bcrypt via this system's own `libxcrypt`
(`crypt_gensalt_rn`/`crypt_r`) -- not a vendored implementation, matching
this project's existing "link a system library" pattern for every other
native dependency rather than introducing a new one. `SecureRandom.bytes`/
`.hex` uses OpenSSL's `RAND_bytes`, already linked for TLS. Both are
class-level "stateless call" natives, the same shape as `Time`/`Process`
(own opcode per method, no new `DIAMOND_OBJECT_*` kind). See
`docs/syntax.md` for the full API and `packages/active_record/README.md`'s
"`has_secure_password`-style password hashing" section for the userspace
helper built on top (`Model#secure_password=`/`#authenticate`, no macro --
same explicit-wiring shape `has_many`/`has_one`/`belongs_to` already use).
General digest hashing (SHA-256 etc.) and HMAC remain undone -- OpenSSL
already provides both via the same linked `libcrypto`, so adding them
later is a small, low-risk extension of this same pattern, not a new
dependency decision.

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
- a general `eval(source) -> value` (compile and run arbitrary source as
  its own isolated program) or anything closing over the *calling*
  scope's own locals -- `ClassName.compile_method` (see "Runtime method
  synthesis" above) is deliberately narrower than either: it only ever
  attaches a new method to an already-loaded class, using `bound_values`
  rather than real closure capture.

## Open design decisions

### Forward and mutual calls

Bare calls resolve only previously declared top-level functions in file order.
Receiver-based method calls resolve dynamically and are the existing workaround
for mutually recursive methods.

Classes/modules/interfaces now go through exactly the declaration-discovery
pass this section used to say fixing this would require -- see "Compiler
representation" above. Top-level bare functions still don't: unlike a class
(an embedded, fixed-size table entry that can be pre-registered by name and
"claimed" later), `program->functions` is a flat, dynamically-growable array
of `DiamondFunction*` shared by every function in the program, top-level or
not, and a `CALL` site bakes in the callee's `function_index` directly, so a
forward call would need that index *reserved* ahead of the callee's own real
compilation, not just its name known -- the same "claim, don't duplicate"
trick, but for a different, currently-unsplittable table. A real fix is more
invasive than the class/module/interface case turned out to be, not just a
smaller version of it.

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
