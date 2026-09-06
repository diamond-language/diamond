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

Prototyped the "compiler append mode" half of this (`diamond_compile_incremental`,
src/compiler.c): compiles new source against an already-compiled `template`
program (e.g. the prelude, compiled once) by seeding both of `diamond_compile`'s
own discovery/real passes with template's classes/interfaces/modules/functions
before either pass runs, instead of re-lexing/re-parsing template's own source
text every call. `tests/run_cases.c` (the 1285-case batch corpus runner) is the
first real consumer: compiles the prelude once at process start instead of on
every one of its ~1285 cases, cutting its own wall-clock time ~25% (measured:
31.0s -> 23.3s, three-run average each, same machine/build). Verified against
the entire existing corpus (1428/1428 tests, byte-identical output) plus a
dedicated regression test (`make test-incremental-compile`) covering prelude-
function/class calls, rejecting a redefined prelude function, sequential reuse
against the same template, forward references within the incremental source
alone, and diagnostic positions matching plain `diamond_compile` exactly.

Deliberately scoped narrow so far: only wired into the batch test runner, which
runs many independent programs per process -- not the `diamond` CLI itself
(one program per process, so there's no in-process template to amortize a
compile against). Fixing the CLI's own cold-start cost (the actual pain point
above) needs the "reusable compiled-prelude snapshot" half instead: generating
`template`'s compiled state once at build time (not runtime) and embedding it
in the binary, skipping prelude-source-parsing entirely rather than doing it
once per process.

Implemented and shipped (`src/compiled_prelude.{h,c}`, `tools/gen_compiled_
prelude.c`, `src/compiled_prelude_data.c`, wired into `diamond_run_source`,
`src/run_source.c`): a same-build-only binary serialization of a compiled
`DiamondProgram`, generated at build time by a throwaway host tool and
`#embed`'d into the `diamond` binary, deserialized via
`diamond_program_read_compiled` into an ordinary, independently-owned
`DiamondProgram` used as a `diamond_compile_incremental` template. The common
case (a program that doesn't need JSON, `diamond_prelude_needs_json`) now
skips lexing/parsing/codegening the prelude's own source entirely on every
`diamond` invocation; a program that does need JSON falls back to the exact
live-compile path this function always used, unchanged (the embedded template
always includes JSON, so using it unconditionally would make a JSON class
name collide with a same-named top-level declaration in a program that never
mentions JSON at all).

Getting this to actually pay off took two follow-up fixes, both found by
measuring rather than assuming a first working version was good enough:

- **First attempt was a net wash.** Wiring the embedded template into the CLI
  the first time made things slightly *worse* (~25-26ms vs. the ~23ms plain
  live compile already took), because `diamond_program_read_compiled` and
  `diamond_compile_incremental`'s own `seed_program_from_template` (which
  clones the template's functions into *both* the discovery and the real
  pass's program) each went through `diamond_function_copy`, which did 6
  separate small `malloc` calls per function -- ~2,800 individual allocations
  for the prelude's 156 functions on a single trivial `-e` invocation. Fixed
  by collapsing those 6 allocations into 1 combined buffer per function
  (`DiamondFunction.owns_combined_buffer`, `src/vm.h`; `diamond_function_copy`,
  `src/compiler.c`) -- but measured again afterward and this alone didn't
  close the gap. Allocation *count* wasn't actually the bottleneck.
- **The real bottleneck: `diamond_program_init`'s own full-struct `memset`.**
  Isolated by timing a *plain* `diamond_compile("1\n", ...)` with no template,
  no prelude, nothing: still ~13-14ms, entirely inside `diamond_program_init`
  being called twice (once for `diamond_compile_impl`'s own throwaway
  `discovery` program, once for the caller's `program`) -- each call's
  `memset(program, 0, sizeof *program)` alone costs ~6.3ms (release build),
  purely from first-touch page faults committing `DiamondProgram`'s ~14.2MB.
  This is a fixed tax on *every* `diamond_compile`/`diamond_compile_
  incremental` call in the entire system, unrelated to source size, template
  use, or anything this feature was originally trying to fix -- it had been
  silently folded into the original "~23ms compiling the prelude" measurement
  all along. Fixed narrowly and safely: `discovery` is always a local
  `calloc(1, sizeof *discovery)` created fresh for exactly one compile call
  and never reused, so `calloc`'s own zero-fill already satisfies
  `diamond_program_init`'s precondition -- its memset there is pure waste.
  Split the memset out from the cheap builtin-class-table setup
  (`diamond_program_init_fresh`, callable directly when the caller can prove
  `program` is freshly `calloc`'d) and used it for `discovery`, plus two other
  always-fresh-never-reused call sites found the same way:
  `clone_program_from_chunk` (Thread.new's own program clone, `src/vm.c`) and
  `allocate_program_builder` (`ClassName.compile_method`/`define_method`'s own
  backing store, `src/vm.c`). `diamond_program_init` itself is untouched for
  every other caller (`tests/run_cases.c`'s own batch loop reuses one
  `DiamondProgram` across 1285+ calls and genuinely needs the full wipe every
  time) -- this was deliberately the *narrow, provably-safe* half of a bigger
  possible fix (see below), not a wholesale rewrite of what needs
  zero-initializing.

Net result (release build, `DIAMOND_TRACE_STARTUP=1`, cold process, repeated
samples): a trivial `-e '1'` (the common, non-JSON case) dropped from ~23-24ms
to ~11.5-13ms total -- roughly a 47-50% cut. The memset fix alone, independent
of the embedded template, also cut the JSON/fallback path (and every other
`diamond_compile` caller in the codebase) from ~23ms to ~15-16ms, a ~35%
reduction that has nothing to do with this feature specifically. Verified
against the full 1428-case corpus, `make test-compiled-prelude`, `make
test-incremental-compile`, `make test-api`, `make test-lsp`, `make test-repl`,
and the fiber/thread suite (`clone_program_from_chunk` changed) -- all pass.

One more real bug surfaced along the way: `compiler_add_function`'s "reopen a
discovery-claimed slot" path (`src/compiler.c`) used to free a function's 6
dynamic arrays individually, but the slot it reopens is always populated
moments earlier by `diamond_compile_impl`'s own "reserve every compiler-
created function" loop -- which, once `diamond_function_copy` started
combining allocations, made that slot combined-allocated on *every* compile,
template or not. Freeing 3 of those 6 pointers as independently owned crashed
immediately (`munmap_chunk: invalid pointer`) the moment the combined
allocation existed. Fixed by routing through a combined-aware free helper
(`diamond_function_free_arrays`).

Also fixed along the way, independent of everything above: `seed_program_from_
template` and `diamond_compile_impl`'s own post-discovery tail both used to
`memcpy` the *entire* fixed-size `classes`/`interfaces`/`modules` arrays
(`DIAMOND_MAX_CLASSES`=180/`DIAMOND_MAX_INTERFACES`=32/`DIAMOND_MAX_MODULES`=32
slots, ~14MB combined) on every single `diamond_compile`/`diamond_compile_
incremental` call, regardless of how many of those slots were actually in
use -- now copies only each table's own used-count prefix.

Real remaining cost, not chased further here: keeping the embedding
infrastructure means `src/compiled_prelude_data.c` (holding the embedded,
uncompressed serialized prelude, ~2.5MB) is swept into every target that
builds `$(SOURCES)`/`$(API_SOURCES)` (the Makefile's own wildcard over
`src/*.c`), which grows the release `diamond` binary from ~2.6MB to ~5.1MB.

The next real lead if this is revisited again: `diamond_program_init`'s
memset is still paid in full for the *caller-supplied* `program` on every
compile (the half deliberately left alone above, since that struct's
zero-state isn't guaranteed the way `discovery`'s local `calloc` is -- `tests/
run_cases.c` genuinely reuses one across 1285+ calls). Removing the need for
that memset entirely -- rather than just skipping it where already-zero can
be proven -- would mean auditing every class/module/interface/function
creation site to confirm each one explicitly zeroes its own new slot's fields
(checked directly for `compile_class`'s "brand new class" branch while
investigating this: it currently does *not*, relying entirely on
`diamond_program_init`'s memset for `method_count`/`field_count`/`class_
variables`/etc. -- removing the memset without first fixing that would
silently reintroduce use of uninitialized, potentially-reused-and-dirty
memory). A bigger, riskier lift than anything done in this pass, deliberately
not attempted here.

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

CI now runs the full test suite across a real 2x2 matrix -- Fedora and Ubuntu
26.04, each with GCC and Clang (`.github/workflows/ci.yml`) -- after a
firsthand pass on real Ubuntu 26.04 + Clang surfaced and fixed three genuine
bugs (Clang's `-O0` giving `run_chunk` a stack frame large enough to defeat
`DIAMOND_MAX_CALL_DEPTH`'s guard, a Fedora-only MariaDB header path, and
`libreginold.a` missing `-fPIC`) plus a test-harness assumption baked into
`timeout`'s exit-status semantics that doesn't hold under Ubuntu's default
`uutils-coreutils` (see CHANGELOG.md).

Both remaining items from this section are done: a full platform abstraction
inventory now lives in docs/portability.md (every OS/libc/kernel/toolchain
assumption found, where it lives, and what's been verified versus assumed),
and a third target -- Alpine (musl), a genuinely different libc, not just a
different distro -- has been validated manually: 1284/1285 corpus cases pass,
the one failure being a real, now-documented libc limitation (`BCrypt.hash`
has no bcrypt implementation to call into on musl), not a bug. Getting there
found and fixed four real, distinct issues (a musl feature-test-macro gap
needing a `#define` in 15 more `.c` files beyond the ones that already had
one for their own reasons, a missing `ucontext.h` *implementation*, not just
declaration, on Alpine specifically, an unassignable global `stdout` on
musl, and a `-fPIE`/`-pie` default mismatch in Alpine's own gcc) -- full
detail in docs/portability.md, which is the right home for this level of
detail going forward rather than growing this section further.

Not yet validated, and worth being honest about rather than letting "we
checked musl" imply more: a non-x86_64 architecture, and any BSD or Darwin
libc. The musl validation itself was also manual (a local container, not
CI) -- making it continuous (a third CI matrix leg) is the natural next
step if musl support is meant to be an ongoing guarantee rather than a
point-in-time check.

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
