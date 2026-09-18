# Diamond roadmap

This roadmap is intentionally forward-looking. Completed work belongs in the
[changelog](../CHANGELOG.md), current behavior in the topic guides under
`docs/`, and implementation rationale in design documents.

Diamond is a research language. Priorities can change when measurements expose
a more valuable runtime, language, or tooling question.

## Current priorities

### Step debugger v2

Both halves landed this cycle (docs/debugging.md): live, no-restart
breakpoints via a new `DIAMOND_OP_BREAKPOINT_CHECK` emitted at every
statement whenever a debug session is compiling at all, and real
`next`/`stepIn`/`stepOut` on top of that same opcode -- turned out to
need no new bytecode or compile-time mechanism at all once every
statement was already instrumented, contrary to this roadmap's own
original prediction that stepping would need "a new bytecode debug-info
format plus deoptimization-style bookkeeping." What's left, if anything:

- **Throttling the live-breakpoint/step poll if it's ever a real
  problem** -- `DIAMOND_OP_BREAKPOINT_CHECK` does a non-blocking
  `poll()` on the control fd at *every* statement to notice a
  `setBreakpoints` sent while running (not currently paused); unthrottled
  today since nothing has shown a need to throttle it. If it ever
  matters, checking only every Nth hit via a counter mask is the same
  well-precedented shape `DIAMOND_RESOURCE_LIMIT_CLOCK_CHECK_MASK`
  already uses for `clock_gettime`.
- **`supportsStepInTargetsRequest`** -- choosing *which* call to step
  into on a line with more than one (e.g. `f(g())`). A real, separate
  DAP feature; basic `stepIn` (into whichever call happens to execute
  first) doesn't need it.

See docs/debugging.md's own "known gap" section too: breakpoints in more
than one `require`d file can still collide on line number, a limitation
of the flat combined-buffer line-number set (both the initial
`DIAMOND_DEBUG_BREAKPOINTS` seed and a live `setBreakpoints` update)
having no file discriminator -- unrelated to and unchanged by either
live breakpoints or stepping. Also see docs/debugging.md's own
"Stepping" section for the one accepted edge case (a self-recursive tail
call doesn't increment the depth counter stepping compares against, so
step-over/out can't fully distinguish it from staying in the same call).

### `struct` declarations

`struct Name(field: Type, ...) ... end` (docs/classes-and-modules.md,
landed this cycle) is deliberately narrow -- see that section's own list
of what it doesn't do. Real possibilities this opens up, none attempted
yet, none committed:

- ~~an additional hand-written body~~ -- **closed** (2026-09, docs/
  classes-and-modules.md's own struct section): `compile_class`'s body-
  parsing loop is now a shared `compile_class_body` (`src/compiler.c`),
  called by `compile_struct` after it registers its own generated
  readers/`initialize`/`==`/`to_s`. A hand-written member can only add,
  not override -- a name collision with a generated method already fails
  via the same duplicate-method checks an ordinary `class` redefining a
  method twice already hits, with no struct-specific handling needed.
- **a superclass, or being reopened** -- both ruled out for this pass
  specifically because there's no obviously correct semantics for
  regenerating `==`/`to_s`/`initialize` against an inherited or changed
  field list; revisit only with a concrete design for what that would
  mean, not speculatively.
- **exact-class `==`** -- the generated `==` accepts any subclass of the
  struct's own class (`IS_TYPE`'s ordinary `is_a?`-style semantics, the
  only building block available without a new opcode), not just an
  exact class match. A struct can't itself have a superclass, but
  nothing stops an ordinary `class Sub < Point` from subclassing one, in
  which case `Point.new(1,2) == sub_instance_with_same_x_y` is `true`
  even though `Sub` may carry extra fields `==` never compares. Narrow,
  known, and not fixed without a real driving need -- Ruby's own naive
  `==` implementations have the identical wrinkle.

### freeze / frozen?

`freeze`/`frozen?` (docs/classes-and-modules.md, landed this cycle) is
deliberately shallow -- freezing a `Hash`/`Array`/`Instance` marks only
that one value, never anything it merely references (an ivar holding a
separate `Array` stays exactly as mutable as it was, matching Ruby's own
shallow-freeze semantics). Real possibilities this opens up, none
attempted yet, none committed:

- **deep/recursive freeze** -- a `freeze(deep: true)`-style option (or a
  separate method) that walks a frozen value's own fields/elements and
  freezes those too. Needs a real cycle-detection story (a frozen
  `Instance` holding an ivar that, transitively, holds a reference back
  to itself) that a plain recursive walk doesn't get for free;
- **frozen string/array literals** -- some languages let a literal itself
  be frozen at the point it's written (`# frozen_string_literal: true` in
  Ruby, or a `%i[]`-style always-frozen literal), avoiding a separate
  `.freeze()` call at every construction site. Diamond's own `String` is
  already unconditionally immutable, so this would only matter for
  `Array`/`Hash` literals; not attempted without a concrete use case
  asking for it.

Revisit only with a real driving need, not speculatively -- same bar
docs/roadmap.md already holds every other research direction to.

### Tail-call optimization

Self-recursive tail-call optimization (docs/callables.md, landed this
cycle) covers only a function's own tail call to itself -- deliberately
narrower than general tail-call elimination, chosen specifically because
self-recursion needs no register-count, chunk, or constant-pool swap
(the callee is always literally the same compiled function already
running). Real possibilities this opens up, none attempted yet, none
committed:

- **mutual tail calls** -- `A` tail-calling a different function `B`
  that in turn tail-calls `A` back (or any statically-resolved direct
  call in tail position to a *different* known function, not just self)
  is real, general tail-call elimination, not just this narrower slice.
  A callee needing a different register count, chunk, or constant pool
  than the caller turns this from "reuse the exact same already-
  allocated `run_chunk` state in place" into "swap out most of the
  current C stack frame's own locals mid-loop," inside `run_chunk`
  itself -- the most performance-audited, most delicate code in the
  codebase. Meaningfully more implementation risk for real, broader
  value;
- **a generic function's own self-tail-call** -- excluded because a
  generic function derives its type-variable bindings fresh from each
  call's own argument values (`run_chunk`'s own `infer_from_value`
  logic, run once at entry); redoing that correctly for an in-place
  looped call, rather than a real recursive re-entry, is a separate,
  unattempted problem;
- **a variadic function's own self-tail-call** -- excluded because a
  variadic call's overflow arguments (beyond its declared fixed
  parameters) are tracked against the *original* call's own `arguments`/
  `argument_count` (`run_chunk`'s own parameters, read directly by
  `DIAMOND_OP_COLLECT_VARIADIC`), not something an in-place "next
  iteration" could actually update without its own new mechanism;
- **surfacing that a call was optimized** -- neither `--dump-bytecode`
  (which does already show `TAIL_CALL` distinctly from `CALL` in
  disassembly) nor any `DIAMOND_TRACE_*` env var currently reports this
  as a runtime event; not attempted without a concrete debugging need
  asking for it.

Revisit only with a real driving need, not speculatively -- same bar
docs/roadmap.md already holds every other research direction to.

### Case/when exhaustiveness checking

Exhaustiveness checking (docs/core-syntax.md, landed this cycle) now covers
two independent closed-type shapes: an explicit union made entirely of
`nil`/user classes, and (added this same cycle, alongside sealed classes --
docs/classes-and-modules.md) a plain type naming a `sealed` class, required
against its own direct subclasses. Both are deliberately conservative in
several ways (see that section's own list). Real possibilities this still
opens up, none attempted yet, none committed:

- **a sealed class's own `when` does not cover its subclasses as a group**
  -- `when Shape` does not count as covering `Circle`/`Square` even though
  `Shape` is exactly the sealed base they both extend (the same exact-
  member-match-only rule an explicit union's own superclass `when` is
  already subject to, applied consistently rather than special-cased for
  the sealed-hierarchy path specifically). Each direct subclass still needs
  its own `when`. Not attempted because it would need to reason about
  "this `when`'s named class is the sealed base itself" as a genuinely
  different, third coverage rule, not just reusing the existing exact-id
  match;
- ~~structural (Array/Hash/Object) pattern coverage~~ -- **closed for
  Object patterns** (2026-09, `CaseArrayNode` in `src/compiler.c`): a
  *single*, top-level, *empty* `Circle{}` class-only guard (no `,`-joined
  alternatives, no reader fields) now counts as covering the `Circle`
  member the same way a bare `when Circle` already did, for both an
  explicit union and a sealed hierarchy (same shared `covered_ids`/
  `exhaustiveness->covered` commit path either way, so no separate change
  needed per shape). A *non-empty* pattern (`Circle{radius: r}`) still
  correctly doesn't count -- it only matches a subset of `Circle` --
  and neither does an empty `Circle{}` nested inside something else
  (`[Circle{}, x]`'s own top-level shape is an Array pattern, not an
  Object one). Array/Hash patterns don't get an analogous rule: unlike
  an Object pattern, they never name a class at all, so there's no
  class id for an "empty `[]`/`{}` covers this member" fact to attach
  to in the first place -- not a narrower cut of the same idea, a
  question that doesn't apply to them;
- **naming the missing member(s) in the compile error** -- Diamond's
  compiler diagnostics are static string literals throughout
  (`DiamondDiagnostic.message` is a raw, non-owning `const char *`, and the
  `Compiler` struct compiling one function is stack-local and gone before a
  caller could read a dynamically-built message out of it), so this needs a
  real, separate diagnostic-message-ownership mechanism benefiting every
  compile error, not something worth inventing for this one check alone.

Revisit only with a real driving need, not speculatively -- same bar
docs/roadmap.md already holds every other research direction to.

### Channel

`Channel` (docs/threads.md, landed this cycle) is a bounded mailbox --
`send`/`receive`/`try_send`/`try_receive`/`close`. Real possibilities this
opens up, none attempted yet, none committed:

- **select-across-multiple-channels** (Go's own `select`) -- receive from
  whichever of several channels has something ready first. Needs a real
  design for waiting on more than one channel's own condition variable at
  once, not just a bigger API surface;
- **unbounded/rendezvous channels** -- `Channel.new(0)`-style synchronous
  handoff, or no capacity limit at all. Deliberately out of v1's own scope
  (a bound keeps memory use predictable and gives `send` real backpressure)
  and not clearly needed without a concrete use case asking for it.

Revisit only with a real driving need, not speculatively -- same bar
docs/roadmap.md already holds every other research direction to.

### `Supervisor`

`Supervisor` (docs/threads.md, landed this cycle) is a flat, `one_for_one`-
only restart-on-crash primitive -- `add_child`/`stop`/`join`/`restart_count`/
`last_error`/`alive?`. Genuine supervision *trees* need no new mechanism (a
supervised child is just a closure free to create and manage its own nested
`Supervisor`, see docs/threads.md's own example), but real possibilities
remain, none attempted yet, none committed:

- **`one_for_all`/`rest_for_one` restart strategies** (Erlang's other two)
  -- restarting every sibling, or every sibling started after the crashed
  one, instead of just the one child. Needs the retry loop to reach across
  sibling children, not just its own slot;
- **configurable restart intensity/backoff** -- v1's fixed 20ms delay
  between a crash and the next restart, with no cap, is a safety valve, not
  a policy; a real "give up after N crashes in M seconds" (Erlang's own
  default) needs an actual policy object, not just a bigger fixed constant;
- **cancel-on-timeout** -- there is no cancellation anywhere in Diamond's
  concurrency model yet (`Thread` doesn't have it either), so this needs
  that more fundamental gap closed first, not something `Supervisor` can
  add on its own;
- **cross-thread-transferable supervisor handles** -- `Supervisor` cannot
  currently cross a `Thread.new`/`Channel` boundary at all (see docs/
  threads.md). Not clearly needed without a concrete use case, since a
  supervised child already can't reference its own parent `Supervisor`
  by design.

Revisit only with a real driving need, not speculatively -- same bar
docs/roadmap.md already holds every other research direction to.

### `diamond build`

`diamond build` (docs/deployment.md, landed this cycle) produces a
standalone native executable via the same serialize/deserialize mechanism
the embedded prelude template already used. Real possibilities this opens
up, none attempted yet, none committed:

- **cross-compilation** -- building for a target other than the machine
  `diamond build` runs on. Needs a real story for cross-linking against
  OpenSSL/SQLite3/`libpq`/MariaDB for the target, not just a compiler flag;
- **a real install step** -- removing the "must run from the repo
  checkout" build-time constraint (`diamond build` invokes `make
  aot-build` directly today, the same assumption `make dap`/`make lsp`
  already make about their own targets) would need Diamond's own headers/
  sources installed somewhere `aot-build` could find without a checkout;
- **static linking** -- a fully hermetic binary with no dynamic dependency
  on OpenSSL/SQLite3/`libpq`/MariaDB/zlib/`libcrypt` at all. Not attempted
  because `diamond` itself doesn't link statically either; doing this for
  `diamond build` alone without doing it for `diamond` would be new,
  unproven build-system work, not a small extension.

Revisit only with a real driving need, not speculatively -- same bar
docs/roadmap.md already holds every other research direction to.

### Sandbox mode

Sandbox mode (docs/sandbox.md, landed this cycle) is a coarse, all-or-nothing switch --
`DIAMOND_SANDBOX`/`diamond --sandbox` deny every native call that opens a real
filesystem/network/subprocess resource, checked directly at each opcode rather than
via a per-VM field, so it applies identically to the top-level program and anything
it spawns (`Thread`, `Supervisor`, `ProgramBuilder#run`) with nothing to propagate.
Real possibilities this opens up, none attempted yet, none committed:

- **per-capability granularity** -- allow network but not filesystem, or allow-list
  specific paths/hosts, instead of one blanket switch. Needs an actual policy format,
  not just more env vars;
- **resource limits** -- a CPU/wall-clock/memory budget per run. This is the other half
  of what docs/internal/fuzzing.md's own execution-fuzzing note asks for ("denying or
  faking out the I/O bridges" is now done; "at least a wall-clock/instruction budget per
  run" is not) -- a real prerequisite for ever attempting execution fuzzing, not
  something sandbox mode itself needs for its own stated purpose;
- **restricting `Thread.new`/`Supervisor.add_child`** -- bounding how many OS threads a
  sandboxed program can spawn (today: the existing process-wide 64-thread cap, same as
  any other program) is a resource-exhaustion concern, not clearly in scope for "deny
  access to the outside world" without a concrete abuse case;
- **restricting `Signal.trap`** -- a process-wide side effect adjacent to, but distinct
  from, resource-opening.

Revisit only with a real driving need, not speculatively -- same bar
docs/roadmap.md already holds every other research direction to.

### Bytecode caching

Bytecode caching (docs/caching.md, landed this cycle) caches a compiled program in a
`.dic` sibling file next to the source, keyed on a SHA-256 hash of the fully
`require`-expanded program and validated against a build fingerprint so a rebuilt/
upgraded `diamond` binary never trusts a stale-format cache. Real possibilities this
opens up, none attempted yet, none committed:

- **a `diamond cache clear`-style command** -- today the only way to force-discard a
  `.dic` file is deleting it by hand or editing the source; not clearly needed without
  someone actually hitting that friction;
- **moving the cache out from next to the source** -- a read-only deployment (a
  container image, a package installed system-wide) can't write a sibling `.dic` file
  at all, silently falling back to "never caches" today rather than failing loudly.
  Something like `XDG_CACHE_HOME`-based storage would fix this but is real, separate
  design work (a different cache key shape, since two different scripts named `app.di`
  in two different directories could no longer be told apart by path alone);
- **caching `diamond -e`** -- excluded in v1 for having no stable on-disk identity to
  cache against; a content-addressed cache in a dedicated directory could cover it, but
  that's the same design work as the point just above, not a small extension.

Revisit only with a real driving need, not speculatively -- same bar
docs/roadmap.md already holds every other research direction to.

### Real semver dependency resolution for `facet` (done)

`facet` used to pin every dependency to an exact git ref (tag/branch/
commit) and treat any two requesters wanting a different ref for the
same cut as a hard, unresolvable error -- even when both refs were
semver-compatible. 0.3's headline ecosystem work replaced that with a
real resolver, still with no hosted registry (cut identity stays a git
URL; "resolving is fetching" stays true) since a git-tag-based resolver
needs none of that operational cost:

- **Semver type**: `tools/semver.c`/`tools/semver.h` --
  MAJOR.MINOR.PATCH[-prerelease][+build] parsing (strict semver.org
  grammar, no leading zeros, an optional leading `v`/`V` for real-world
  git tags) and precedence ordering, plus range/constraint syntax
  (`^1.2.3`, `~1.2.3`, `>=1.0.0 <2.0.0`, an exact `1.2.3`), satisfaction
  checks, and range intersection. Standalone -- no dependency on `facet`
  itself or the Diamond compiler/VM, since it's also generally useful on
  its own (a package's own `diamond.cut` `version` field finally means
  something). `make test-semver` (80 cases) covers parsing, ordering,
  ranges, satisfaction, and intersection.
- **Version discovery without a registry**: `git ls-remote --tags --refs`
  against a dependency's own repository, filtered to tags that parse as
  semver (with or without a leading `v`) -- the entire "version
  database" this ever consults. No index, no service, no caching layer
  beyond what git itself already does.
- **Manifest format**: a `dependencies` entry can now carry a `version`
  key as an alternative to `tag`/`branch`/`commit` (mutually exclusive
  with those, the same way the existing ref keys are mutually exclusive
  with each other) -- `{"git": "...", "version": "^1.2.0"}`.
- **The resolver**: a version-constrained dependency can't resolve the
  moment it's seen the way an exact-ref one does -- with no registry,
  discovering a cut's own transitive dependencies needs a clone of some
  concrete version of it, but *which* version depends on every
  requester's constraint, and some requesters aren't discovered until
  later in the walk. So `tools/facet.c` now keeps exact-ref dependencies
  on the original clone-immediately-and-recurse path, but parks a
  version-constrained name in a pending table instead, intersecting in
  each new requester's own constraint as it's found; a queue-plus-
  pending-table fixpoint (`resolve_full_graph`) alternates draining
  newly-discovered exact-ref work and resolving one pending name (tags
  listed, highest match picked, cloned, its own dependencies queued)
  until both are empty. Picking the highest match is deterministic, no
  real backtracking -- an empty intersection is a hard error naming
  every requester and its own range; mixing an exact ref and a version
  constraint for the same cut is also a hard error, except when whichever
  side resolved first happens to already satisfy the other's constraint
  too, in which case there's nothing to reconcile.
- **Lockfile**: `facet.lock` still pins to one exact resolved commit
  regardless of whether the manifest asked for an exact ref or a range;
  a range-resolved entry also records which tag it resolved to, for
  transparency (`facet update` always re-resolves from `diamond.cut`
  fresh either way, never consulting the old lockfile's own pinned
  version).
- Two real, previously-undiscovered bugs found and fixed along the way,
  by finally building `facet` under ASan/UBSan/LeakSanitizer for the
  first time in this file's history: `facet_run_hash` passed freshly
  `malloc`'d (not zeroed) memory to `diamond_compile`, which requires an
  already-valid `DiamondProgram` since its own first act is freeing
  whatever was there before recompiling -- crashed on the very first
  manifest read, reproduced identically on facet.c from before this
  work. `facet_program_free` never called `diamond_program_free` on the
  compiled program's own internal arrays before freeing the outer
  struct, leaking them on every manifest/lockfile read (harmless in
  practice -- `facet` is a short-lived CLI process -- but a real,
  fixable leak).
- **Still explicitly out of scope**: a hosted registry/index (install by
  bare name, search) and multi-version coexistence -- both deferred for
  good reason (docs/packages.md's own "architecturally constrained"
  note: two versions of one cut could never coexist in Diamond's single
  flat compiled namespace anyway) and neither changes with this work.
- **Real backtracking landed (2026-09)**: re-resolving an already-cloned
  version-constrained dependency when a later-discovered constraint it
  doesn't satisfy shows up used to be an unconditional hard error, even
  when a different, still-available tag would have satisfied every
  requester. `resolve_full_graph` now retries the whole graph walk
  (wiping the ephemeral scratch clones each time) whenever that happens,
  carrying forward the full intersected constraint history for every
  version-constrained name across attempts -- so a later attempt already
  knows everything an earlier one discovered, however late, and resolves
  correctly the first time it reaches that name. Bounded at
  `FACET_MAX_DEPENDENCIES` attempts, a real provable limit (one restart
  per genuinely new conflicting-constraint discovery, and there are at
  most that many version-constrained edges in the whole graph), not a
  guess. A genuinely disjoint pair of constraints (no tag could ever
  satisfy both, independent of resolution order) still hard-errors
  immediately, on the first attempt -- no restart wasted on an
  unsatisfiable graph. Scoped to pure version-vs-version conflicts only;
  mixing an exact ref and a version constraint for the same name is
  still `docs/packages.md`'s own by-design hard error, unrelated to this.

0.3 overall is a bugfix-and-ecosystem release: this packaging work was
the headline addition, alongside whatever bugs turn up along the way
(see "Harden the end-user runtime surface" below for the audit already
in progress) rather than new language surface.

### Improve receiver-aware tooling

The Language Server understands many statically visible receiver types,
including constructors, annotated values, `self`, unions, and selected inferred
assignments. It still recompiles complete documents and loses precision across
some dependency and dynamic-flow boundaries.

`textDocument/references` (lsp/references.c) now covers one piece of
"dependency-aware symbols across a workspace": a workspace-wide,
conservative, name-based scan for every call/access/type-position/
declaration occurrence of a top-level function/class/module/interface,
reusing `workspace/symbol`'s own file-walking. It does not change how a
single document's own receiver facts are computed -- that per-document
compile is unaffected, and cross-file precision within one document's own
`require` closure was already sound (everything required is inlined into
one compiled unit; Diamond's `require` model has no way to reference a
class that isn't).

`textDocument/rename` (also lsp/references.c) reuses that same
`find_workspace_occurrences` scan to rewrite every occurrence together as
one `WorkspaceEdit`, rather than adding a second, independent workspace
walk.

Unannotated call chains (`def make_branch() = Branch.new()` used as
`make_branch().leaf()`) are now resolved too: `compile_definition` (src/
compiler.c) infers a function's own return type from its body_result the
same way `compile_block` already did for blocks, into a new
`inferred_return_type_set` kept deliberately separate from
`return_type_set` itself (which real compile-time semantics -- interface
conformance, generics -- depend on) so this stays a pure, zero-risk
tooling improvement. See docs/lsp.md's receiver-chain paragraph.

Control-flow joins across function boundaries are now covered too: a
function whose body branches into more than one class with no shared
annotation had no single inferable body-result type before, even where
the equivalent local-variable case (an `if`/ternary assigned to a local)
already synthesized a union. A bare trailing `if`/`case` expression
already got this for free (merge_flow_types, the same control-flow-join
primitive an `if`/`case`'s own per-branch merge already uses, writes the
merged set straight onto that expression's own destination register
unconditionally) -- the real remaining gap was a function using explicit
`return` statements across separate branches instead, which body_result
alone can never see. `compile_return` (src/compiler.c) now accumulates
every explicit `return value`'s own type into a new `return_flow_seen`/
`return_flow_type`/`return_flow_set` running union (the same incremental-
accumulate shape `merge_loop_exit` already uses for a loop's own `break`
values, minus the locals/alias-identity bookkeeping a `return` doesn't
need), saved/restored around a nested function body the same way every
other per-function compiler field already is; `compile_definition` unions
that into the trailing-expression inference above rather than replacing
it, so a function mixing both styles infers correctly either way.

Next steps:

- improve receiver facts across imported files further where they still
  lose precision. One concrete dynamic-flow boundary is now closed: an
  unannotated imported function or method's inferred result survives
  assignment to a local for hover/definition/completion, including an
  instance-method call through an already inferred local, via metadata kept
  separate from compile-time type semantics. `if`/`unless` joins now retain
  the same fact across `if`/`unless`, `case`, and loop exits and clear
  conflicting facts; independently cloned but structurally equivalent facts
  merge as the same receiver graph. Other unrepresentable dynamic results
  remain deliberately unresolved. An inferred union receiver also carries an assigned method
  result when every class arm has a usable declared or inferred return;
  generic functions, instance methods, and singleton methods do likewise when
  every type variable resolves;
  directly chained generic calls resolve explicit class-union bindings,
  including unions nested in class-element `Array` shapes, and class-union
  argument inference;
  captured/boxed locals preserve the same tooling-only fact through the cell;
  grouping parentheses preserve any otherwise resolvable receiver expression;
  repeated indexing from a nested class-element `Array` local preserves the element
  receiver fact, including a structurally known non-generic top-level,
  singleton, or single-class instance-method call as the array source
  (including generic calls with class-union argument inference or
  matching parameterized shapes such as `Array[T]` and `Hash[String, T]`, and
  explicit class-union or nested class-union `Array` bindings;
  nullable `Hash` indexing remains conservative);
  union receivers preserve indexed method results when every candidate has an
  equivalent non-generic structural array return;
- explore incremental compilation only after the compiler has a reusable unit
  boundary that makes incremental synchronization worthwhile;
- keep editor results conservative when a receiver cannot be proven.

### LSP formatting

`textDocument/formatting` (docs/lsp.md, landed this cycle) normalizes each
physical line's own leading indentation and trims trailing whitespace,
tracking nesting depth from the same token stream every other LSP handler
here already uses -- deliberately not a full AST-based pretty-printer or
reflow engine, since the native compiler retains no AST to reflow against
at all (see "Compiler representation" below). Real possibilities this
opens up, none attempted yet, none committed:

- **operator/comma spacing normalization** -- consistent spacing around
  binary operators, after commas, etc. Would need its own token-adjacency
  heuristic, independent of the indentation-depth tracking this landed
  with;
- **blank-line collapsing** -- capping runs of consecutive blank lines.
  Left alone this pass as a purely cosmetic judgment call with no single
  correct answer the way indentation depth has;
- **`textDocument/rangeFormatting`** -- formatting a selection rather than
  a whole document. The same line-by-line depth algorithm would need a
  starting depth handed in from outside the selection, which nothing
  currently tracks.

Revisit only with a real driving need, not speculatively -- same bar
docs/roadmap.md already holds every other research direction to.

### Harden the end-user runtime surface

The native service layer is broad enough to build real applications. Work here
should now favor consistency, portability, and failure behavior over adding
unrelated primitives.

A first arity/type/range/closed-resource audit (2026-09) across File,
Listener, Socket, UDPSocket, TLSSocket, SQLite3/Statement, PostgreSQL,
MySQL, and Process/Handle/Stream found the surface largely consistent
already: closed-resource checks are structurally guaranteed (every
native dispatch checks "is closed" before any method-specific branch),
arity validation is shared correctly across sibling methods (e.g.
`Process.run`/`.spawn` share one argv-validation helper), and the one
real type-consistency question found (PostgreSQL/MySQL rejecting a Hash
`params` argument that SQLite3 accepts) turned out to be a genuine,
already-documented driver-level constraint, not an oversight. One real
range-check inconsistency was found and fixed: `UDPSocket#receive`
rejected `0` where the read-family methods didn't.

Re-run (2026-09-15) against every native surface added since the first
pass -- `Tensor`, `Channel`, `Supervisor` -- the same way: sibling-method
arity/type/range consistency (`Tensor#get`/`#set` share one arity/bounds
check before their own method-specific work; `Supervisor#restart_count`/
`#last_error`/`#alive?` all validate their shared child-index argument
identically), constructor-level range/overflow guards (`Tensor.zeros`/
`.random` both reject non-positive rows/cols; `allocate_tensor` itself
guards `rows*cols*sizeof(double)` against overflow before ever calling
`malloc`, so an absurd shape fails cleanly with `OUT_OF_MEMORY` rather
than wrapping into a too-small allocation), and cross-type error-code
consistency (`Channel#try_send`/`#try_receive`'s `WouldBlockError` is
the exact same status non-blocking `Socket`/`File` reads already use,
not a parallel invented one). Found nothing to fix this time -- a real,
useful outcome in its own right, not just a formality: confirms the
audit's own bar was met by three independently-authored features
without anyone deliberately checking against it at the time each shipped.
Re-run this audit periodically as new native surface is added, rather
than treating it as permanently closed.

The focused ThreadSanitizer suite was re-run in 2026-09 across the thread,
channel, supervisor, and representative fiber cases: 35 cases passed with no
races reported.

Minor-GC stress now covers representative Channel payload transfer, nested
channels, Supervisor restart, and nested supervision cases; the shared batch
runner clears `DIAMOND_STRESS_MINOR_GC` between cases so those settings cannot
leak across the corpus.

TCP, TLS, and subprocess timeout/error cleanup was audited in 2026-09. Failed
connect/listen/handshake and pipe-setup paths close transient descriptors, while
rooted TLS handles retain sole ownership for later sweep cleanup; the existing
timeout and failure cases passed without an ownership defect.

SQLite resource cleanup is now stress-tested under both major and minor GC for
prepared-statement reuse/closure and a statement that outlives its closed
connection.

Live-driver validation also passed in 2026-09 against throwaway PostgreSQL 16,
MariaDB 11, and MySQL 8 containers: all three existing Arel dialect suites
passed 12/12 tests.

Those three opt-in scripts now enable both major and minor GC stress by
default; the live suites passed 36/36 tests under forced collection.

SQLite's deterministic invalid-query and closed-resource error fixtures also
run under combined major/minor-GC stress, covering cleanup on rejected
operations as well as successful statement lifetimes.

The live PostgreSQL, MariaDB, and MySQL suites now include the same invalid-
query/use-after-close assertions and pass 13/13 each under combined stress.

Priorities:

- continue stress-GC, sanitizer, thread, socket, TLS, subprocess, and database
  coverage;
- document platform-dependent behavior explicitly;
- preserve thread safety: per-value state is preferred to mutation of
  process-global settings.

### Grow packages from application needs

Diamond already includes HTTP/Rack-style infrastructure, SQL construction,
persistence, GraphQL, authentication, cookies, logging, and example
applications. New package work should be driven by a concrete application or
interoperability need.

Likely directions:

- deepen ActiveRecord and Arel only where applications expose a missing query,
  association, migration, or dialect feature;
- add another SQL dialect only with a live server for verification;
- improve package documentation, examples, and compatibility tests;
- avoid framework magic that hides database access or weakens Diamond's type
  and error contracts.

## Runtime research

### Bound pause time further

The collector is generational mark/sweep with remembered sets and card marking.
Collections are still stop-the-world within each isolated VM.

Potential research:

- measure pause distributions on long-lived application workloads;
- tune promotion and collection thresholds using evidence;
- investigate incremental marking only if pause measurements justify the added
  barrier and state-machine complexity.

### Native-code execution

The register bytecode VM has inline caches, object shapes, specialization, and
opt-in quickening. A first, deliberately narrow baseline JIT now exists
(`src/jit.c`/`src/jit.h`, opt-in via `DIAMOND_JIT=1`, see
[`docs/internal/jit-design.md`](internal/jit-design.md) for the full design
and phase history) -- it compiles call/argument/self-passing, primitive
arithmetic and comparisons, `SUPER`, and Hash/String/Array-backed ivar and
index access, each via a hand-verified trampoline extracted from the real
interpreter's own opcode body rather than a general codegen pipeline.
Measured, real wins on `bench/RESULTS.md`'s own benchmarks (release build):
~2.9-3x on `int_arithmetic.di`, ~6-8% on `hash_ivar_construct.di`, ~4-8% on
`object_hydration.di`.

**It never reached its original motivating target, and the real reason
turned out to be three separate gaps, not one.** The actual goal was
skindicate's own `Model#initialize`-shaped row hydration. Landed one
narrow, real, independently-useful piece (2026-09, `docs/internal/jit-
design.md`'s own "Phase 2f"): `.length()`/`.to_i()`/`.ord()`/etc. now
publish their known fixed scalar return type at compile time (reusing
`DIAMOND_NATIVE_METHODS`, a table already built for interface
conformance checking), so a `.length()`-bounded comparison gets
`LESS_INT` from its very first execution under the plain interpreter,
with no dependence on `DIAMOND_QUICKEN` ever kicking in -- corrected
2026-09-15 to also cover a plain `String`/`Array`/`Hash` receiver with
no registered type set (originally only fired for Array/Hash, which
always get one for unrelated element-tracking reasons; a bare `String`
local never did), see `docs/internal/jit-design.md`'s own Phase 2f
correction note. **This is an
interpreter-level win only, confirmed not to move JIT eligibility at
all** -- investigated end to end, empirically, not just by re-reading
the opcode whitelist: `Model#initialize`-shaped code remains
unconditionally JIT-ineligible for three independent reasons:

- ~~`jc->has_called`'s own compile-time-only, monotonic nature~~ --
  **closed** (2026-09, `docs/internal/jit-design.md`'s own "Phase 3"):
  the real blocker wasn't the flag itself, it was that `ADD_INT`/
  `SUBTRACT_INT`/`MULTIPLY_INT`/`DIVIDE_INT`/`LESS_INT`'s own edge cases
  (a non-Int operand, overflow, division by zero, `INT64_MIN`/`-1`) had
  no *resumable* fallback -- their only option was "discard and retry
  the whole function," unsafe once a call had already run. New shared
  trampolines (`diamond_jit_arith_slow`/`diamond_jit_compare_slow`,
  extracted from the interpreter's own case bodies, `src/vm.c`) give
  them the same "call a trampoline, never retry" shape every other
  call-capable opcode here already has, so they no longer need
  `has_called` to be false at all -- verified against the exact shape
  named below (a Hash `INDEX_GET` followed by a loop counter's own
  `ADD_INT`/`LESS_INT`, `tests/cases/jit_int_arith_after_index_get.*`),
  which failed to compile before this change and compiles cleanly
  after;
- ~~`DIAMOND_OP_GET_IVAR` has no case in the JIT's own compile-time
  opcode scan at all~~ -- **closed** (2026-09, `docs/internal/jit-
  design.md`'s own "Phase 2g"): a new `diamond_jit_get_ivar` trampoline,
  mirroring `SET_IVAR`'s own existing one exactly (same field-cache
  lookup, no `has_called`/frame needed -- plain field access can never
  invoke user code or allocate). A method that only reads/writes its
  own ivars plus does int arithmetic is now fully JIT-eligible where it
  wasn't before (verified via `DIAMOND_TRACE_JIT`, `tests/cases/jit_
  get_ivar.*`), though `Model#initialize`-shaped code specifically still
  isn't (see below -- its loop body's real `INDEX_GET`/`INDEX_SET` calls
  are the remaining blocker, not ivar access);
- ~~generic `DIAMOND_OP_INVOKE`~~ -- **partially closed, and reframed**
  (2026-09, `docs/internal/jit-design.md`'s own "Phase 4"). Reading the
  real case body found it's ~2740 lines (`src/vm.c`, the full native-
  method dispatch table for every builtin type plus Instance dispatch
  with inline caching), not one extractable case -- "generic `INVOKE`
  support" was never a realistically scoped single phase, and this
  roadmap's own prior wording undersold that. Separately, the real
  motivating target had already changed: `Model#initialize`
  (`packages/active_record/lib/active_record/model.di`) no longer loops
  over `.keys()`/`.length()` -- the 2026-09-15 ORM hydration hotspot fix
  already on record rewrote it to `attributes.dup()`. JIT support for
  just the three receiver-kind-agnostic pseudo-methods `dup`/`freeze`/
  `frozen?` (never able to invoke arbitrary user code, unlike every
  other `INVOKE` shape) closes that gap for `Model#initialize` *as it
  exists today* -- verified via `DIAMOND_TRACE_JIT` showing it compile
  where it didn't before, and a real, if modest, ~8-10% end-to-end
  timing win on an updated `bench/object_hydration.di`
  (`tests/cases/jit_invoke_dup.*`). Every other `INVOKE` shape --
  every native per-type method (`.keys()`/`.length()`/...), all Instance
  method dispatch, `tap`, `public_send` -- remains completely
  unattempted, and is now understood to be a dramatically larger,
  separately-scoped undertaking than "one more trampoline."

`Model#initialize` as it exists today is now fully JIT-eligible end to
end -- the original three-gap picture (`has_called`, `GET_IVAR`, generic
`INVOKE`) is closed for that specific function's own current shape. A
future rewrite of `Model#initialize` that reintroduces a `.keys()`-style
loop, or any method calling a native per-type method or another
user-defined method, is not covered by any of this -- true generic
method dispatch from JIT'd code remains an open, unattempted, and now
much better-understood-in-scope research direction, not a committed
feature.

**Generic (non-`_INT`) arithmetic/comparison now compiles too** (2026-09,
`docs/internal/jit-design.md`'s own "Phase 5") -- found while
investigating why a Hash-lookup-and-sum workload never JIT-compiled at
all: one generic `ADD`/`SUBTRACT`/`MULTIPLY`/`DIVIDE`/`LESS`/
`LESS_EQUAL`/`GREATER`/`GREATER_EQUAL` anywhere in a function (any value
the compiler can't statically prove `Int` -- an untyped parameter, a
Hash/Array element) used to bail the *entire function* out of JIT
eligibility, with zero prior support for the generic forms at all. Turned
out to need no new trampoline: Phase 3's own slow-path functions already
handled being called with a generic opcode correctly. Real, measured
wins: `bench/int_arithmetic_dynamic.di` (the file that first named the
typed-vs-untyped gap) ~2.1x faster JIT'd, closing most of its own ~2.8x
gap against the fully-typed `int_arithmetic.di`; `bench/hash_ops.di`
~1.5x. **Also found, empirically, a real bug already live in production**:
`emit_epilogue_propagate` unconditionally popped a `DiamondFrame` on the
belief that a compiled function's own `has_called` flag implied a frame
had been pushed -- false since Phase 2e, not just Phase 5 -- corrupting
`vm->frames` (a real segfault, reproduced directly on the pre-Phase-5
commit already deployed to skindicate.dia) whenever a `has_called`-
without-`needs_frame` opcode was followed by a genuinely error-
propagating one, called repeatedly. Fixed alongside this phase; see
`docs/internal/jit-design.md`'s own Phase 5 section for the full
timeline and why it went unnoticed through every prior phase's own
verification pass.

Before extending past the current narrow slice:

- identify hot workloads that remain VM-bound after existing
  specialization *and* after the current JIT's own whitelist (a
  workload similar to `Model#initialize` needs all three gaps above
  closed together, not just one);
- define deoptimization and GC-root contracts for anything that compiles a
  call, allocation, or exception path the current slice deliberately avoids;
- require benchmark evidence large enough to justify the added complexity,
  the same bar the current slice was itself held to.

### Compiler representation

The native compiler emits bytecode directly and intentionally does not retain a
general AST. A stable intermediate representation could help optimization,
tooling, or incremental compilation, but would add substantial complexity.

Revisit only when at least one concrete consumer can state the required
invariants and demonstrate that current bytecode/source metadata is
insufficient.

## Language directions

### Ergonomics without compatibility chasing

Ruby-inspired syntax should remain familiar, but Diamond is not a Ruby
compatibility implementation. New syntax and core methods should satisfy a
demonstrated Diamond use case and have clear typing, dispatch, and error
semantics.

Areas worth considering:

- smaller application-driven collection and string conveniences;
- calendar helpers that fit the existing UTC/local/fixed-offset model;
- explicit metaprogramming operations with inspectable behavior;
- better ways to express common typed callback and data-shaping patterns.

### Non-x86_64 architecture validation

Native Linux arm64 CI now runs the standard `make test` baseline plus focused
API, incremental-compile, compiled-prelude, fiber, LSP, and REPL targets on a
real GitHub-hosted arm64 runner (`test-arm64`). That closes the original
compiler-only portability gap: fibers, dynamic loading, atomics, alignment,
endianness assumptions, subprocess behavior, and editor tooling all execute on
the target architecture on every push. A separate `test-arm64-sanitize` job
also runs the full ASan/UBSan suite natively; it is kept separate because its
24-minute instrumented build and run nearly exhausted the baseline job's
30-minute budget on the first combined attempt. The baseline job additionally
runs five representative package suites natively (database configuration,
HTTP, Gremlin, Rack, and GraphQL), covering configuration/filesystem work,
TCP/TLS, threaded serving, middleware, and application-level parsing without
requiring an external service. Coverage remains intentionally narrower than
`test-all`: TSan, Redis/external-database integration, and the remaining package
suites have not yet been validated on Linux arm64 and should be widened only as
separate measured increments.

The arm64, musl, and macOS jobs exclude the JIT cases while still compiling
the JIT source and exercising the interpreter. The current JIT is validated
only on glibc x86-64: setting `DIAMOND_JIT` elsewhere safely leaves it
disabled instead of executing an unvalidated machine-code/ABI combination.
Adding portable native backends remains a separate performance project, not a
prerequisite for interpreter portability.

### Stable public boundaries

The language, bytecode format, embedding API, and package conventions are still
unstable. Versioning should follow proven external consumers rather than
freezing experimental interfaces prematurely.

A first stability pass should cover:

- source compatibility promises;
- bytecode validation and versioning;
- native embedding ownership and error contracts;
- package layout and lockfile compatibility.

## Tooling and distribution

### Self-hosted frontend

The Diamond-written lexer and compiler are useful differential oracles and can
bootstrap through themselves. The native C compiler remains the production
frontend.

Keep the self-hosted implementation at practical parity where it protects the
language, but do not duplicate every native optimization unless it advances a
specific bootstrap or language-design goal.

### Portability

CI covers Fedora, Ubuntu 26.04, Alpine (musl), FreeBSD, macOS/Darwin, and
native Linux arm64 -- three libcs, three kernels, and two architectures --
across GCC and (for the two glibc x86-64 targets) Clang
(`.github/workflows/ci.yml`, `test-all`/`test-musl`/`test-freebsd`/
`test-macos`/`test-arm64`/`test-arm64-sanitize` jobs). Full inventory of every OS/
libc/kernel/toolchain assumption checked, fixed, or found absent:
docs/portability.md.

Remaining, genuinely open: OpenBSD is blocked on a real gap (no
`<ucontext.h>` at all, a Fiber-implementation limitation, not an unwritten
CI job) -- see docs/portability.md's "FreeBSD/OpenBSD" section. Debian
itself is also untested (Ubuntu is the glibc target CI actually runs). The
musl/FreeBSD/macOS/arm64 jobs are each narrower than the two-glibc-distro one
on purpose (GCC-only for musl/arm64, Clang-only for FreeBSD/macOS, no
sanitizers on musl/FreeBSD/macOS, no TSan on arm64, and no package-specific
tests on musl/FreeBSD/macOS while arm64 runs only a representative package
slice) -- widening any of them needs that coverage actually checked first, not
just enabled.

## Explicitly deferred

- Ruby compatibility as a goal;
- named IANA timezone parsing bundled into the VM;
- shared mutable heaps between OS threads;
- free-form runtime source evaluation (`ClassName.compile_method` compiles a
  source string into a single capture-free method for `define_method` --
  see docs/classes-and-modules.md -- but that's not a general `eval`: no
  calling-scope closures, no naming other classes, and no arbitrary
  expression evaluation outside a method body);
- a hosted package registry without an operational owner;
- a JIT without representative profiling evidence;
- portability claims without continuous testing on the claimed platform;
- runtime class *synthesis* (decided, 2026-09): `DIAMOND_VALUE_CLASS`
  stays exactly as narrow as it is today (only `self` inside a
  class-owned singleton method and a bare class name as a `case`/`when`
  pattern produce one -- both compile-time-resolved special forms, not
  general expressions; `.class()` returns a diagnostic `String`, not a
  Class value, and `is_a?` never touches one either). A class's identity
  is a `uint8_t class_index` (`DIAMOND_MAX_CLASSES = 180`) sharing the
  exact byte space the whole static type system uses for `known_type`/
  type sets/generics -- a class and a compile-time type are the same
  representation, deliberately. Synthesizing new classes at runtime would
  force a second, untyped object-model tier outside that system entirely,
  against "dynamic code and checked code share one object model"
  (README.md); not worth it without a deeper type-tag redesign no
  concrete use case currently justifies. See docs/internal/design.md's
  `DIAMOND_VALUE_CLASS` section for the implementation-level detail.

## Completion policy

When roadmap work lands:

1. document the end-user behavior in the appropriate guide;
2. record the capability as a concise changelog milestone;
3. keep detailed measurements or rationale in a focused design/benchmark
   document when they remain useful;
4. remove the completed item from this roadmap.
