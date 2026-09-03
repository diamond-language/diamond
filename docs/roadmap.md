# Diamond roadmap

This roadmap is intentionally forward-looking. Completed work belongs in the
[changelog](../CHANGELOG.md), current behavior in the topic guides under
`docs/`, and implementation rationale in design documents.

Diamond is a research language. Priorities can change when measurements expose
a more valuable runtime, language, or tooling question.

## Current priorities

### Make programs start faster

The embedded Diamond prelude is compiled with every program. Source selection
already avoids loading JSON support when it is unused, but the remaining core
still adds measurable startup cost.

Measured (release build, `DIAMOND_TRACE_STARTUP=1`, warm page cache): a
trivial `-e '1'` spends ~23ms of its ~24ms total wall time compiling the
23.7KB non-JSON prelude -- over 95% of process time before the program's own
code runs at all. Adding JSON (36KB combined) or a require-heavy program
(`tests/cases/arel_traversal.di`, 202KB combined with its requires) keeps the
same shape: compile is consistently 90-96% of total wall time and scales
roughly linearly with combined source size, while `load` (require resolution)
and `run` (actual execution) stay in the single-digit milliseconds or less.
Process overhead outside compilation (`diamond -v`) is unmeasurably small.
This is a fixed tax paid identically on every invocation regardless of
program size -- most costly for the CLI/test-suite/short-script pattern
(the 1000+ program end-to-end corpus, for one, pays it 1000+ times over) and
irrelevant to a long-running server's steady state.

Next investigation:

- prototype either a reusable compiled-prelude snapshot or a compiler append
  mode, now backed by the measurement above rather than a hypothesis;
- keep the new machinery only if it produces a meaningful measured gain without
  making source maps, diagnostics, or embedding substantially more fragile.

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

Priorities:

- audit native APIs for consistent arity, type, range, and closed-resource
  errors;
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

### Runtime classes and metaprogramming

Classes are not general heap values, and Diamond has no `eval`-style runtime
compiler. Existing method definition/replacement APIs operate on already
compiled callables.

Possible future work:

- decide whether class objects should become ordinary values;
- define inheritance/cache invalidation rules before expanding runtime method
  synthesis;
- consider `method_missing` only with bounded recursion, visibility, cache, and
  diagnostic semantics.

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

Diamond currently targets Linux with GCC and POSIX facilities. Portability work
should begin with a documented platform abstraction inventory and CI on a
second real target, not scattered conditional compilation without validation.

## Explicitly deferred

- Ruby compatibility as a goal;
- named IANA timezone parsing bundled into the VM;
- shared mutable heaps between OS threads;
- free-form runtime source evaluation;
- a hosted package registry without an operational owner;
- a JIT without representative profiling evidence;
- portability claims without continuous testing on the claimed platform.

## Completion policy

When roadmap work lands:

1. document the end-user behavior in the appropriate guide;
2. record the capability as a concise changelog milestone;
3. keep detailed measurements or rationale in a focused design/benchmark
   document when they remain useful;
4. remove the completed item from this roadmap.
