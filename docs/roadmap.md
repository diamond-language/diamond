# Diamond roadmap

This roadmap is intentionally forward-looking. Completed work belongs in the
[changelog](../CHANGELOG.md), current behavior in the topic guides under
`docs/`, and implementation rationale in design documents.

Diamond is a research language. Priorities can change when measurements expose
a more valuable runtime, language, or tooling question.

## Current priorities

### Real semver dependency resolution for `facet`

`facet` currently pins every dependency to an exact git ref (tag/branch/
commit) and treats any two requesters wanting a different ref for the
same cut as a hard, unresolvable error (docs/packages.md) -- even when
both refs are semver-compatible. 0.3's headline ecosystem work is a real
resolver, still with no hosted registry (cut identity stays a git URL;
"resolving is fetching" stays true) since a git-tag-based resolver needs
none of that operational cost:

- **Semver type**: MAJOR.MINOR.PATCH[-prerelease][+build] parsing and
  ordering, plus range/constraint syntax (`^1.2.3`, `~1.2.3`, `>=1.0.0
  <2.0.0`, an exact `1.2.3`) -- a standalone piece with no dependency on
  `facet` itself, since it's also generally useful on its own (a
  package's own `diamond.cut` `version` field finally means something).
- **Version discovery without a registry**: a dependency's available
  versions come from `git ls-remote --tags` against its own repo,
  filtered to tags that parse as semver (with or without a leading `v`).
  No index, no service, no caching layer beyond what git itself
  already does.
- **Manifest format**: a `dependencies` entry gains a `version` key as
  an alternative to `tag`/`branch`/`commit` (mutually exclusive with
  those, the same way today's own ref keys are mutually exclusive with
  each other) -- `{"git": "...", "version": "^1.2.0"}`.
- **The resolver**: collect every requester's own constraint for a given
  cut name. If every constraint is an exact ref (today's model,
  unchanged), they must all match exactly -- same hard-error-on-mismatch
  behavior as today. If constraints are semver ranges, intersect them
  and pick the *highest* available tag satisfying the intersection --
  deterministic, no real backtracking needed for a first version. An
  empty intersection is a hard error naming every requester and its own
  range (a real improvement over today's bare "different ref" message).
  Mixing an exact-ref constraint with a semver-range constraint for the
  same cut is also a hard error -- there's no principled way to compare
  an arbitrary commit against a range's intent.
- **Lockfile**: `facet.lock` keeps pinning to one exact resolved commit
  regardless of whether the manifest asked for an exact ref or a range
  (reproducibility doesn't change); a range-resolved entry also records
  which tag/version it resolved to, so `facet update` has something to
  compare the *next* resolution against.
- **Still explicitly out of scope**: a hosted registry/index (install by
  bare name, search) and multi-version coexistence -- both previously
  deferred for good reason (docs/packages.md's own "architecturally
  constrained" note: two versions of one cut could never coexist in
  Diamond's single flat compiled namespace anyway) and neither changes
  with this work.

0.3 overall is a bugfix-and-ecosystem release: this packaging work is
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

Next steps:

- control-flow joins across function boundaries: a function whose body
  branches into more than one class with no shared annotation (an
  unannotated `if`/`case` returning different classes per arm) still has
  no single inferable body-result type and stays unresolved -- the local-
  variable version of this (an `if`/ternary assigned to a local) already
  synthesizes a union; doing the same for a whole function's own inferred
  return type is the natural next slice, not yet attempted;
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

### Self-hosted frontend

The Diamond-written lexer and compiler are useful differential oracles and can
bootstrap through themselves. The native C compiler remains the production
frontend.

Keep the self-hosted implementation at practical parity where it protects the
language, but do not duplicate every native optimization unless it advances a
specific bootstrap or language-design goal.

### Portability

CI covers Fedora, Ubuntu 26.04, and Alpine (musl) -- two libcs, not just two
distros -- across GCC and (for the two glibc targets) Clang
(`.github/workflows/ci.yml`, `test-all`/`test-musl` jobs). Full inventory of
every OS/libc/kernel/toolchain assumption checked, fixed, or found absent:
docs/portability.md.

Remaining, genuinely open: no non-x86_64 architecture and no BSD or Darwin
libc has been validated at all. The musl job is also narrower than the
glibc one on purpose (GCC only, no sanitizer/tsan builds, no package-
specific tests) -- widening it needs those actually checked first, not
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
