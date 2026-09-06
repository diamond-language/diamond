# Diamond roadmap

This roadmap is intentionally forward-looking. Completed work belongs in the
[changelog](../CHANGELOG.md), current behavior in the topic guides under
`docs/`, and implementation rationale in design documents.

Diamond is a research language. Priorities can change when measurements expose
a more valuable runtime, language, or tooling question.

## Current priorities

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

Next steps:

- improve receiver facts across imported files (unannotated call chains,
  control-flow joins across function boundaries);
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

### Package distribution

`facet` installs git-pinned packages and writes a lockfile. A hosted registry,
semantic-version solver, signing model, and multi-version dependency graph are
deferred until real package distribution needs justify their operational cost.

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
  concrete use case currently justifies. See docs/design.md's
  `DIAMOND_VALUE_CLASS` section for the implementation-level detail.

## Completion policy

When roadmap work lands:

1. document the end-user behavior in the appropriate guide;
2. record the capability as a concise changelog milestone;
3. keep detailed measurements or rationale in a focused design/benchmark
   document when they remain useful;
4. remove the completed item from this roadmap.
