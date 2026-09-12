# Diamond roadmap

This roadmap is intentionally forward-looking. Completed work belongs in the
[changelog](../CHANGELOG.md), current behavior in the topic guides under
`docs/`, and implementation rationale in design documents.

Diamond is a research language. Priorities can change when measurements expose
a more valuable runtime, language, or tooling question.

## Current priorities

### Channel: what's next, if anything

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

### `Supervisor`: what's next, if anything

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

### `diamond build`: what's next, if anything

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

### Sandbox mode: what's next, if anything

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

### Bytecode caching: what's next, if anything

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
  Also not attempted: re-resolving an already-cloned version-constrained
  dependency when a later-discovered constraint it doesn't satisfy shows
  up (a real backtracking problem, hard-errors instead of guessing).

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
  lose precision (dynamic-flow boundaries this pass didn't touch);
- explore incremental compilation only after the compiler has a reusable unit
  boundary that makes incremental synchronization worthwhile;
- keep editor results conservative when a receiver cannot be proven.

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
rejected `0` where the read-family methods didn't. Re-run this audit
periodically as new native surface is added, rather than treating it as
permanently closed.

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
opt-in quickening. A tracing or method JIT remains an open research direction,
not a committed feature.

Before implementation:

- identify hot workloads that remain VM-bound after existing specialization;
- define deoptimization and GC-root contracts;
- require benchmark evidence large enough to justify a second execution tier.

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

### Step debugger v2: live breakpoints and real stepping

v1 (`dap/`, docs/debugging.md) ships editor gutter breakpoints, a real
call stack, and locals at the paused frame -- entirely by compiling a
`DIAMOND_OP_DEBUGGER` pause in at each breakpoint's own line before the
debuggee starts, reusing `debugger()`/`breakpoint()`'s existing pause
machinery untouched. Two limitations were taken on deliberately to keep
that scope low-risk (no new bytecode, no changes to `run_chunk`'s own
dispatch loop):

- no step-over/into/out -- `continue` is the only resume command;
- changing a breakpoint means restarting the whole debuggee -- there is
  no way to add or remove one against an already-running process.

Both need real new mechanism, not a bigger v1: a per-instruction
breakpoint-check hook inside `run_chunk`'s hot dispatch loop (the most
performance-audited code in the project) for live, no-restart
breakpoints, and a new bytecode debug-info format plus deoptimization-
style bookkeeping for stepping. Revisit only with a concrete need driving
the added dispatch-loop risk -- not attempted speculatively. See
docs/debugging.md's own "known gap" section too: breakpoints in more than
one `require`d file can currently collide on line number, a v1 limitation
of the flat `DIAMOND_DEBUG_BREAKPOINTS` set having no file discriminator,
separate from either limitation above.

### Self-hosted frontend

The Diamond-written lexer and compiler are useful differential oracles and can
bootstrap through themselves. The native C compiler remains the production
frontend.

Keep the self-hosted implementation at practical parity where it protects the
language, but do not duplicate every native optimization unless it advances a
specific bootstrap or language-design goal.

### Portability

CI covers Fedora, Ubuntu 26.04, Alpine (musl), FreeBSD, and macOS/Darwin --
three libcs and three kernels, not just five distros -- across GCC and (for
the two glibc targets) Clang (`.github/workflows/ci.yml`, `test-all`/
`test-musl`/`test-freebsd`/`test-macos` jobs). Full inventory of every OS/
libc/kernel/toolchain assumption checked, fixed, or found absent:
docs/portability.md.

Remaining, genuinely open: no non-x86_64 architecture has been validated,
and OpenBSD is blocked on a real gap (no `<ucontext.h>` at all, a Fiber-
implementation limitation, not an unwritten CI job) -- see
docs/portability.md's "FreeBSD/OpenBSD" section. Debian itself is also
untested (Ubuntu is the glibc target CI actually runs). The musl/FreeBSD/
macOS jobs are each narrower than the two-glibc-distro one on purpose
(GCC-only for musl, Clang-only for FreeBSD/macOS, no sanitizer/tsan
builds, no package-specific tests) -- widening any of them needs that
coverage actually checked first, not just enabled.

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
