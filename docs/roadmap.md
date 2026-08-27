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
variable known at the cursor position to hold `ClassName.new(...)`
(`DiamondScopeLocal` plus ordered `DiamondScopeTypeFact` assignment snapshots),
and now also a union receiver -- a parameter (or a
local initialized from one) given an explicit `pet: Dog | Cat` annotation
(`DiamondScopeLocal.known_type_set`, the same snapshot idea applied to
`known_type_sets`, decoded against the owning function's own `type_sets[]`).
Resolves against every class-kind union member that defines the method: one
signature when they agree, `ClassName#signature` per match when they don't,
a `Location[]` from go-to-definition when there's more than one match.
The compiler now synthesizes advisory unions at representable `if`/`unless`
and ternary joins, so a branching assignment such as
`x = cond ? Dog.new() : Cat.new()` is resolvable alongside an explicit source
annotation. These inferred sets can eliminate a compatible runtime type check,
but deliberately retain the historical dynamic check when incompatible rather
than changing old code into a compile error. Class instance-variable receivers
resolve too when all assignments to the field agree on one concrete class; an unknown or conflicting assignment
conservatively disables the result. Explicitly typed call results now compose
recursively as receivers as well: constructors, top-level factories, singleton
factories, instance methods, and class-union returns can all feed the next link
in a chain. An unannotated return or one unusable union arm stops resolution.
See `docs/lsp.md` and `lsp/receiver.c` for the mechanism and its scope cuts.

Remaining, still open:

- dependency-aware symbol information beyond one combined compilation;
- incremental compilation only after there is a compiler architecture that can
  benefit from incremental document synchronization.

Loop exits now participate in the same conservative flow analysis as
conditionals and `case`: `while`/`until` join the zero-iteration state, the
completed-body state, and every `break`, while an unconditional `loop` joins
its reachable `break` values and local states. This preserves class unions for
receiver tooling and proves compatible loop-expression return annotations.

## Self-hosting: minimal-compat maintenance mode

The Diamond compiler can compile and run itself (self-parse and self-run
bootstrap, `tests/self_host_smoke.sh`), but full parity is intentionally
deferred. The native language is still gaining features, so maintaining a
second hand-ported frontend in lockstep would repeatedly duplicate work.
Self-hosting will resume after the native surface has settled, allowing the
self-hosted compiler to adopt the accumulated syntax, diagnostic, and opcode
features in one deliberate consolidation pass.

While paused:

- forward-reference gaps in the self-hosted frontend may require declaration
  ordering that the native compiler does not. New prelude types should retain
  bootstrap-compatible ordering for now; implementing parity immediately is
  deferred so it can arrive with the planned consolidated language-feature
  adoption rather than as another isolated compatibility patch;
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

Resolved for per-function bytecode: the former 4,096-byte ceiling was only a
fixed-array storage choice, while every jump target was already encoded as an
unsigned 16-bit absolute offset. `DiamondFunction` now grows its bytecode and
line/column maps geometrically, up to the real 65,535-byte wire-format boundary.
Discovery-pass reservations and thread-local program clones deep-copy these
buffers, and program teardown owns them explicitly. This also removes roughly
36 KiB of unconditional storage from every small function record; functions
pay only for the bytecode capacity they actually reach.

The VM's quickening rewrite-site ledger now grows dynamically as well. Keeping
that ledger indexed by the enlarged bytecode maximum would otherwise have
added roughly 480 KiB to every VM, including stack-resident and nested VMs.
If ledger growth fails, the candidate call site simply remains unquickened.

Resolved next for literal constants: `DIAMOND_OP_CONSTANT` already decoded a
16-bit index, but `add_constant` narrowed it to `uint8_t` and every function
reserved a fixed 256-value table. Constant storage now grows geometrically to
the real 65,535-entry operand boundary, including ProgramBuilder emission,
discovery reservations, thread clones, and teardown. This removes another
roughly 4 KiB of unconditional storage per function.

Resolved for type sets: graph links, function and interface contracts, runtime
collection constraints, compiler flow facts, ProgramBuilder metadata, and all
explicitly typed call operands now carry 16-bit indices. The compiler, VM,
disassembler, self-hosted parser, LSP, and thread-clone paths share that wire
format. The dedicated `0xffff` "no type set" sentinel replaces the legacy
`0xff` marker, recovering slot 255; real annotations occupy every index from
0 through 65,534. Type tables grow geometrically, interface metadata is rebound
after entry-table
relocation, and thread clones own a separate entry type graph rather than
retaining pointers into the parent program.

Resolved for string/name operands: storage grows dynamically and the bytecode
wire format now carries unsigned 16-bit indices through literals, symbols,
method dispatch, spread and keyword calls, `super`, debugger metadata, and VM
synthetic call frames. ProgramBuilder emission, discovery reservations, thread
clones, the disassembler, and the self-hosted parser share the same layout.
Functions can therefore address up to 65,535 string/name entries without
restoring the former roughly 64 KiB unconditional allocation per function.

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
a rediscovered name. See `tests/cases/forward_declarations.di`. Top-level
function slots are likewise reserved at their discovery-pass indices and
claimed in the same order by the real pass, so calls and callable references
can target later declarations without destabilizing class/module method
indices. See `tests/cases/forward_top_level_calls.di`. This is
*not* a general predeclaration/IR change -- there is still no retained
AST, and a superclass/base-interface still needs to be declared first
(that copies the referenced declaration's already-*fully-compiled* field
table, not just its name -- seeing it registered isn't enough, see
`tests/cases/forward_declaration_superclass_still_fails.di`).

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

Resolved calls now propagate declared return graphs through every statically
known top-level, singleton, and instance dispatch form. Generic results
substitute explicit or inferred positional, keyword, and homogeneous-spread
bindings into caller-owned graphs; nested indexing, generic unions, and typed
Callable-value results continue that flow through chained expressions.

Inference now recurses through Callable parameter/return graphs, including
Callables nested in collections. Union receivers publish generic results only
when every implementation shares a structurally identical generic signature
and return contract.

Fixed-arity bound instance methods and explicitly bound generic instance or
singleton references now carry wrapper-owned parameter and return graphs.
Callable unions publish their result when independently stored return graphs are
structurally equivalent.

Variadic bound-method wrappers now preserve their fixed prefix and return graph,
and bound Callable keyword calls use the original method parameter names.
Callable unions provide trailing-block context when their nested contracts agree
structurally.

Callable-union context now also accepts a declared nested contract that can
satisfy every arm under parameter contravariance and return covariance. Bound
wrappers preserve omitted optional arguments through argument-count branches,
leaving default evaluation in the original method.

Context-driven generic method-reference inference remains open. Today a bare
generic reference is compiled before a later call or parameter contract is
known, and the no-AST single-pass compiler has no expected-type channel flowing
back into that earlier expression. Implementing it requires an explicit
bidirectional context mechanism rather than guessing bindings from a future
consumer. General synthesis of a new common Callable contract remains separate;
the current variance path deliberately selects an existing declared candidate
and falls back when no arm is safe for all others.

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

- **Done**: strict nested Array binding patterns in `case`/`when`, combining
  exact recursive shape checks, `_` wildcards, lowercase bindings, and the
  existing literal/Range/Regexp/class/custom-equality matchers without partial
  binding on failed patterns;
- **Done**: nested strict Array destructuring assignment with bracketed
  patterns (`[head, [left, right]] = value`), including mixed local/instance/
  class-variable leaves and exact shape checks at every level;
- **Done**: trailing rest patterns (`[head, *tail]`) for destructuring
  assignment and `case` Array patterns, including nested use, empty tails,
  `*_` discard, minimum-arity checks, and fresh remainder Arrays;
- **Done**: middle Array rest patterns (`[head, *middle, tail]`) in assignment
  and `case`, with suffix-relative extraction and nested/empty spans;
- **Done**: required-key Hash destructuring assignment with nested Array/Hash
  targets, `**remaining` capture, and failure-atomic stores;
- **Done**: `^local` pins in nested `case` Array patterns, comparing against
  existing local or captured values without rebinding them;
- **Done**: required-key Hash binding patterns in `case`, including nested
  Array/Hash values, wildcards, pins, extra-key tolerance, and atomic bindings;
- **Done**: trailing Hash rest patterns (`**remaining` and `**_`) with fresh
  unmatched-entry Hashes, nested extraction, and final-position enforcement;
- **Done**: class-guarded object binding patterns in `case`, extracting public
  zero-argument readers into nested patterns with atomic binding commits;
- **Done**: `when pattern if condition` guards, with provisional binding
  visibility, match-first evaluation, and mutation-free guard fallthrough;
- **Done**: comma-separated binding-pattern alternatives with identical-name
  validation, ordered short-circuit matching, shared guards, and atomic commits;
- **Done**: subjectless boolean `case`, with truthiness-based clauses,
  short-circuit alternatives, guards, and the existing conservative flow joins;
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
- **Done**: `[]`/`[]=` operator overloading. Previously a deliberate scope
  cut (`docs/syntax.md` used to say outright "aren't overloadable") --
  revisited once a real consumer (`packages/active_record`'s own
  `DirtyAttributes`, which had been using `#get`/`#set` specifically to
  work around this) made the gap concrete. Reuses the exact same
  `invoke_operator_method` dispatch every other overloadable operator
  already goes through (`src/vm.c`) -- generalized from a single
  optional argument to up to two, since `[]=` needs both the index and
  the value -- so inheritance/`super`/interface-satisfaction come for
  free, the same as `+`/`==`/`<=>`/etc. already have. `compile_definition`
  (`src/compiler.c`) gained the one new piece: `[]`/`[]=` aren't single
  lexer tokens the way every other operator name is, so recognizing
  `def [](i)`/`def []=(i, v)` needed its own adjacent-token lookahead,
  requiring no whitespace between `[`/`]`/`=` (matching every other
  operator name here being one ungappable token). No compiler changes
  were needed for `x[i] += v` or chained `x[a][b] = v` at all --
  confirmed directly, not assumed: `compile_index_assignment`/
  `compile_index_compound_assignment` already emit plain `INDEX_GET`/
  `INDEX_SET` regardless of receiver type, so both compose correctly the
  instant the VM opcodes themselves know how to dispatch to an Instance.
  `<<` deliberately did **not** move -- out of scope for this change, see
  its own paragraph in `docs/syntax.md`;
- **Done**: protected method visibility, including default and named forms,
  explicit peer receivers, inheritance, and unrelated-caller rejection;
- **Done**: a first lazy Enumerator pipeline with deferred, composable
  `map`/`select`/`reject` transforms and `each`/`to_a`/`force` terminals;
- **Done**: source-level native collection extension bridges using
  `array_`/`hash_`/`enumerable_` naming conventions, eliminating VM edits for
  new collection methods while retaining native-operation precedence.
- **Done**: qualified class names in `case` and object patterns, such as
  `when GraphQL::Language::Field` and
  `when GraphQL::Language::Field{name: name}`. Qualified names work in `is`
  checks. Both class-only matching and qualified object destructuring now use
  the same namespace-aware class resolution, and the namespaced GraphQL
  type-dispatch ladders have been converted to `case`.
- **Done**: short-circuit lazy terminals (`take`, `find`, `any?`, and `all?`).
  `each_until` is the non-exceptional iteration-control protocol. Arrays,
  Hash values, and Ranges stop at the source; custom Enumerable sources can
  override the default protocol to provide the same cooperative early stop.
- **Done**: generalized spread calls, including fixed prefix/suffix arguments,
  Callable values, constructors, class/module singleton methods, explicit
  generic bindings, fixed-plus-rest delegation, and runtime keyword binding
  for functions, user-defined methods, Callable values, constructors, and
  class/module singleton methods.
- **Done**: keywords for native C-backed receiver methods. One central native
  signature registry supplies stable public parameter names; bound arguments
  re-enter `INVOKE_SPREAD`, keeping arity, defaults, types, and method behavior
  in the original native dispatch branches.
- **Done**: spread calls on native receivers. Native String/Array/Hash/resource
  methods remain specialized branches inside `INVOKE`; the spread opcode
  constructs a compact synthetic call frame and re-enters that one dispatch
  matrix. This avoids a divergent copy while preserving universal-method,
  built-in, and source-level collection-extension precedence. Native receiver
  spreads inherit ordinary native invocation's existing 16-argument bound.
- **Done**: bound instance-method references. `receiver.method` captures the
  receiver once in a variadic Callable and forwards through dynamic spread
  invocation; explicit generic bindings, native receivers, inheritance,
  visibility, and `method_missing` share their ordinary dispatch behavior.
- **Done**: variadic and explicitly generic singleton-method references.
  Zero-capture wrappers forward collected rest arguments through singleton
  spread and bake explicit type bindings into typed call instructions; unbound
  generic references remain a compile error by design.
- **Done**: comma-separated literal destructuring right-hand sides. Values are
  evaluated left to right, assembled into one Array, and then use the existing
  recursive, failure-atomic shape validation and store pass.

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
since it *is* one -- not a parallel dispatch model. Parameters are bare names
only (no type annotations or defaults). A trailing `*arguments` parameter
provides variadic forwarding through mixed instance-method spread, including
fixed-plus-rest signatures. A final `&block` parameter forwards an optional
trailing block through Diamond's ordinary last-Callable-argument convention.
Splat and block forwarding can be combined: the variadic prologue preserves
the final block slot while collecting only the arguments between fixed
parameters and that block.
The same optional `&block` parameter and explicit forwarding marker are now
available to ordinary functions and methods, including calls shaped as
`target(*arguments, &block)`; blocks remain ordinary Callable values at runtime.
Source block closures carry an independent block tag, so `*arguments, &block`
can distinguish a supplied block from an ordinary final Callable or any other
rest value. Variadic block parameters therefore default cleanly to `nil`, and
generated delegates omit the block from target dispatch when it is absent.
Ordinary function, method, and Callable spread calls now preserve the explicit
forwarding marker as well: `target(*arguments, &block)` omits a nil block while
leaving ordinary trailing nil and Callable values untouched.
Functions and methods declaring `&block` now gain lexical `yield(args...)`
invocation and `block_given?()` presence checks. The same token retains its
fiber-suspension meaning in bodies without an explicit block parameter, keeping
the two runtime mechanisms separate without a compatibility break.
Explicit `Fiber.yield(value)` and `Fiber.yield()` now provide suspension from
any lexical context. Callable values also accept trailing source blocks for
fixed and spread calls. Constructor calls now do the same for `initialize`;
keyword Callable, dynamic-method, and constructor calls now reconcile the
trailing block against the target's final public parameter slot as well.
Explicit optional block parameters now accept `Callable` annotations. Their
checks run only for supplied blocks and after `*arguments` collection when both
features share one signature.
Typed `yield` expressions now propagate the annotated Callable return set.
Anonymous blocks infer a concrete return set from a statically known final
expression. Concrete parameter types from one-member Callable annotations now
flow into anonymous blocks at statically resolved top-level, singleton,
instance-method, and constructor call sites. Typed Callable values propagate
their final nested Callable parameter through fixed, spread, and keyword calls.
Explicit generic bindings substitute concrete block parameter types at function
and method call sites. When bindings are omitted, direct one-member generic
parameters now infer them from concrete positional arguments for statically
resolved top-level, singleton, and instance-method calls. Array and Hash
literals retain recursive compile-time contracts and join distinguishable
member types into conservative unions, allowing direct and nested collection
parameters to contribute bindings too. Fixed constructor
calls infer the generic bindings declared by `initialize` through the same
path. Homogeneously typed spread Arrays provide bindings for top-level,
singleton, instance-method, and constructor calls, including merged fixed
arguments that retain the same element contract.
Keyword arguments at statically resolved calls bind against their declared
parameter slots for top-level, singleton, instance-method, and constructor
calls. Union receivers participate when every member resolves either to the
same inherited implementation or to divergent implementations with structurally
identical arity, generic, name, and parameter contracts. Unions may retain
multiple members with the same outer Array, Hash, or Callable identity when
their nested contracts differ; exact structural duplicates remain errors.
Incompatible overrides and genuinely unresolved generic contexts remain future
extensions to the same conservative mechanism.
Typed top-level function references and class/module singleton references now
publish structural Callable signatures into local type facts, so later fixed,
spread, and keyword Callable-value calls retain block context through aliases.
Trailing blocks may now bypass defaulted positional parameters immediately
before `&block`. Zero-keyword block calls reuse the keyword-normalization path,
which inserts an internal undefined sentinel for each omitted optional slot.
`run_chunk` leaves that register nil while `ARGUMENT_PROVIDED` reports false,
so the callee's existing default-value prologue runs without exposing a new
source-language value.
Compiled functions now retain whether their final parameter used `&`, allowing
LSP hover to distinguish optional blocks from defaults and locate a preceding
variadic slot correctly. Hover also recognizes `block_given?()` as a Boolean
lexical intrinsic.
The forwarded call always uses the same name declared
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
delegation in this slice. `delegate foo(*arguments), to: @target` now emits
an ordinary variadic method whose collected Array is forwarded through
`DIAMOND_OP_BUILD_SPREAD_ARGS` and `DIAMOND_OP_INVOKE_SPREAD`.
`delegate_missing_to` additionally
depends on a general missing-method protocol. Both remain separate
future design questions.

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
`delegate`'s own existing scope cut.

### `method_missing`

**Done, in scope**: `def method_missing(name, args)` is now consulted at
the existing dispatch-miss branch (`lookup_method`/`lookup_method_cached`
already being the single choke point every ordinary instance-call call
site shares) instead of always raising immediately -- a class with no
`method_missing` still gets the same failure it always did, now surfaced
as a dedicated `NoMethodError` (a new `StandardError` subclass) rather
than a generic `TypeError`. See `docs/design.md`'s "`method_missing`"
section for the full mechanism and scope cuts.

Confirmed directly, not assumed: a `DIAMOND_STRESS_GC=1` run surfaced a
real heap-use-after-free (an unrooted `Symbol` collected out from under
itself by the very next allocation) that the non-stress run's matching
output completely hid -- fixed via `gc_protect`, and now part of why
every new dispatch helper allocating more than one fresh heap value before
they're all reachable from a single register/container gets this same
scrutiny going forward.

**Deliberately not in scope**: only the ordinary instance-method dispatch
site -- not operator overloading, not `#to_s`, not `super`, not
`self.`-singleton dispatch, each of which already has its own sensible
fallback that silent redirection would more likely surprise than help.

### `closure name() ... end`

**Done, in scope**: resolves "Open design decisions"' own former "Nested
`def` closures don't capture `self`/ivars" entry -- of the three sketched
options there, went with a new explicit keyword rather than escape-based
inference (too implicit for this language's stated values) or a modifier
on the existing form (still two compiled shapes behind one easy-to-forget
flag). `closure name() ... end` captures `self` (and therefore `@ivar`,
`self.foo(...)`) alongside ordinary outer locals, exactly for the
immediately-invoked-closure case plain nested `def` never handled; plain
nested `def` itself is completely unchanged, still the
`define_method`/`redefine_method` patch-factory form. See
`docs/design.md`'s "`closure name() ... end`" section for the full
mechanism (no VM/opcode changes, no `DiamondClosure` field additions --
self capture rides in the existing `captures[]`/`capture_count` array
like any other captured value) and `docs/syntax.md`'s own section for the
language-level shape and examples.

Surfaced by this feature's own work (motivated by `active_record`'s own
migration-runner use case) and fixed separately, in the same session:
a nested `def`/`closure` redeclared a second time inside the same loop
body used to raise a runtime `TypeError` at the second declaration -- see
"Locals captured inside a loop body go stale" below.

### Locals captured inside a loop body go stale

**Done, in scope.** A loop body is compiled once and every iteration
after the first reaches it via a jump back to that same bytecode -- a
local captured by a nested `def`/`closure` partway through the body gets
its register boxed in place, and any reference to it compiled *before*
that capture was discovered stayed a stale raw-register read that only
the lucky first iteration (box hasn't happened yet) survived. Confirmed
directly, and confirmed broader than the originally-reported "redeclared
closure" framing: an ordinary body statement referencing an outer
pre-loop-declared local has the same problem with no closure
redeclaration in sight (`loop do ... break if ... end`), and so does a
local declared fresh *inside* the loop body, captured further down in
the same body, independent of any outer local. Fixed by a cheap
lookahead at the top of any `while`/`until`/`loop` that detects a
`def`/`closure` anywhere in that loop's own body and, if found,
preemptively marks every already-visible local -- and every local
declared from then on -- as captured, so they all get the existing
box-aware codegen from their very first compilation instead of only
from wherever the actual capturing `def`/`closure` happens to sit. No
VM/opcode changes, no changes to `break`/`next`/`redo`'s jump targets
(confirmed unnecessary, not just skipped). See `docs/design.md`'s
"Locals captured inside a loop body go stale" section for the full
mechanism.

**Deliberately narrower than exhaustive, confirmed rather than assumed**:
covers plain assignment (`define_local`, the sole site for
`x = value`-style declarations) and pre-existing locals, which are the
two failure modes actually reproduced. Seven other, rarer local-
registration sites (destructuring, `rescue error:` bindings, parameter
binding, and others) aren't touched -- a local declared via one of those
forms inside a capturing loop, then itself captured later in the same
body, remains unfixed. Revisit if that shape shows up in practice.

### `closure` with declared parameters collided with self capture

**Done, in scope.** Also surfaced by `active_record`'s migration-runner
use case, and separate from the loop-staleness bug above: passing a
zero-arg `closure` to a `Callable[0]`-typed parameter worked correctly
as soon as the `closure` feature itself landed, but a `closure` that
also declares its own parameter(s) didn't -- its self-materialization
and its first declared parameter both landed in register 0, since a
`closure` kept `owner_class==UINT8_MAX` (deliberately, to stay excluded
from `redefine_method`'s installation path) and every calling
convention's implicit-receiver-skip decision is keyed off that same
field. Fixed with a third `owner_class` sentinel value, distinct from
"not a method" and the existing module-method sentinel, that gets a
`closure` the register-0 skip everywhere it's needed while still
permanently failing `redefine_method`'s own exact-match installation
check -- plus a paired fix in `call_closure_helper` (`src/vm.c`, the
shared mechanism behind calling any `Callable` *value* directly) so a
`closure`'s arity-bounds check uses its own real, un-inflated declared
arity rather than the padded count used only for register placement.
See `docs/design.md`'s "`closure` with its own declared parameters
collided with self capture" section for the full mechanism.

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
SHA-256 and HMAC-SHA-256 are now exposed as `Digest.sha256` and
`HMAC.sha256`, returning lowercase hexadecimal strings and preserving raw
String bytes. Additional algorithms remain demand-driven rather than becoming
an open-ended crypto-wrapper surface.

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

### Forward and mutual calls (resolved)

Bare top-level calls now resolve later declarations, including mutual
recursion, generic/keyword calls, callable references, and calls crossing
`require`-expanded files. The discovery pass tolerates an as-yet unknown bare
call or function value and records all functions. Before the real pass, every
function is copied into the same numeric slot with `declared_by_discovery`;
compiler-created functions then claim and clear those slots in source order.
This preserves the `function_index` baked into `CALL` instructions and the
indices already stored in copied class/module method tables. The real pass
still performs ordinary undefined-function, arity, keyword, and type checks.

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

### Nested `def` closures don't capture `self`/ivars -- resolved

Was an open question in this section; resolved and shipped this session
as a new explicit keyword (the option this section originally favored
over escape-based inference or a modifier on the existing form) -- see
"`closure name() ... end`" under "Language and library directions" above
for what landed, and `docs/design.md`'s own section by that name for the
full mechanism.

### Stable compiler boundary

`ProgramBuilder` exists to support the self-hosted bootstrap. It is not a stable
embedding API. A public compiler API would need explicit ownership, source-map,
versioning, and cross-program type semantics rather than exposing the current
internal structure by accident.
