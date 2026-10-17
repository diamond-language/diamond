# Diamond roadmap

This roadmap tracks possible work. Completed work belongs in the
[changelog](../CHANGELOG.md), current behavior in the topic guides, and
implementation rationale in design documents. A section here says what is not
done yet; when something lands, delete it (see "Completion policy").

Diamond is a research language. Priorities can change when measurements expose
a more valuable runtime, language, or tooling question.

## Candidate work

These are possible next steps, not commitments. Start with a measured need or
a concrete application gap.

### `struct` declarations

Current behavior: [classes and modules](classes-and-modules.md).

- **Being reopened** -- no obviously correct semantics for regenerating
  `==`/`to_s`/`initialize` against a changed field list. Revisit only with a concrete design.
- **Fields added by a subclass in the generated `==`** -- the generated `==` compares only
  the struct's own declared fields, so two instances of a subclass that differ only in a
  field the subclass added compare equal unless that subclass defines its own `==`.
  Comparing every instance field would fix it but needs equality dispatch for nested
  values; fix only with a driving need.

### Channel

Current behavior: [threads](threads.md).

- **Rendezvous channels** -- `Channel.new(0)`-style handoff, where `send` returns only
  once a receiver has taken the value. Needs a concrete use case. An unbounded
  channel is deliberately not planned: the capacity bound is what keeps memory
  predictable and gives `send` backpressure.

### `Supervisor`

Current behavior: [threads](threads.md).

- **Interrupting the last blocked workers** -- the host-name lookup inside `MySQL.open` and a
  read from the process's own standard input are still only stopped between instructions by
  `Supervisor#stop(timeout_ms)`. Standard input may be shared with the parent, so making it
  non-blocking needs a different technique than the owned-descriptor one. Implicit
  cancellation checkpoints are open (see [cancellation](cancellation.md)).
- **Cross-thread-transferable supervisor handles** -- a `Supervisor` cannot cross a
  `Thread.new`/`Channel` boundary, and a supervised child cannot reference its parent
  by design. Needs a concrete use case.

### `diamond build`

Current behavior: [deployment](deployment.md).

- **Cross-compile CI** -- the cross kit is validated by hand against an aarch64 sysroot
  under qemu; a CI job would need a cached sysroot and an emulator.
- **Targets beyond 64-bit little-endian** -- a 32-bit or big-endian target needs a
  portable program image rather than the current struct dump.
- **MySQL in static builds** -- a static musl kit leaves the MariaDB client out; including
  it needs Connector/C built from source as a static archive.
- **Static output for glibc and non-Linux targets** -- only musl/Linux is supported;
  `diamond` itself does not link statically.

### Sandbox mode

Current behavior: [sandbox](sandbox.md).

- **Path and host allow-listing** -- capabilities are whole categories; finer
  matching needs a policy format, not more environment variables.

### Improve receiver-aware tooling

Current behavior: [language server](lsp.md). The language server recompiles complete
documents and loses precision across some dependency and dynamic-flow boundaries.

- Improve receiver facts across imported files where they still lose precision:
  element graphs through callback-dependent collection transforms, methods
  whose result varies, unrepresentable dynamic results, and nullable `Hash`
  indexing remain unresolved.
- Explore incremental compilation only after the compiler has a reusable unit boundary
  that makes incremental synchronization worthwhile.
- Keep editor results conservative when a receiver cannot be proven.

### Harden the end-user runtime surface

The native service layer is broad enough to build real applications. Work here should
favor consistency, portability, and failure behavior over adding unrelated primitives.
Results and test commands belong in the topic guides and test scripts.

- Continue stress-GC, sanitizer, thread, socket, TLS, subprocess, and database coverage;
  re-run focused audits as new native services land.
- Document platform-dependent behavior explicitly.
- Preserve thread safety: per-value state is preferred to mutation of process-global
  settings.

### Grow packages from application needs

New package work should be driven by a concrete application or interoperability need.

- Deepen ActiveRecord and Arel only where applications expose a missing query,
  association, migration, or dialect feature.
- Finish the ORM return-annotation audit. Extensible Arel query/node returns need a
  protocol before they can be narrowed, and `Relation#first`, `Repository#find` and the
  `Model` finders return a mapper-produced value or `nil`, which no `-> Type` can express
  without generics. The JIT never trusts an unannotated return; measure each batch rather
  than assuming a gain (see
  [JIT design](internal/jit-design.md#where-the-next-win-is)).
- Add another SQL dialect only with a live server for verification.
- Improve package documentation, examples, and compatibility tests.
- Avoid framework magic that hides database access or weakens Diamond's type and error
  contracts.

## Runtime research

### Bound pause time further

Collections are stop-the-world within each isolated VM.

- Measure pause distributions on long-lived application workloads.
- Tune promotion and collection thresholds using evidence.
- Investigate incremental marking only if pause measurements justify the added barrier
  and state-machine complexity.

### WebAssembly

Current state: [WebAssembly build](internal/wasm.md).

- **Fibers** -- need stack switching (Asyncify or the stack-switching proposal).

### Native-code execution

Current behavior and phase history: [JIT design](internal/jit-design.md).

- **Generic `DIAMOND_OP_INVOKE` beyond the receivers already proven** -- most native
  per-type methods (`.keys()`, `.slice()`, ...), and Instance dispatch on a receiver
  with no compile-time class proof.
- **Resuming mid-function after a fallback** -- `dup`/`freeze`/`frozen?`/generic
  arithmetic reached after an earlier call-capable opcode, where the receiver is not the
  expected shape at run time, cannot resume mid-function. Closing it needs the
  resumable-fallback design arithmetic already has, generalized.
- **Return-value receivers with no compile-time class** -- needs a byte-offset-to-PC
  mapping to make the language server's per-register type-fact table usable from
  `src/jit.c`, or an equivalent new mechanism.
- **Non-x86-64 backends** -- the JIT is validated only on glibc x86-64 and stays
  disabled elsewhere. Portable native backends are a separate performance project, not a
  prerequisite for interpreter portability.
- Before extending any of this: find hot workloads that remain VM-bound after the
  existing specialization and the current whitelist, and require benchmark evidence
  large enough to justify the added complexity.

### Interpreter source structure

Current layout and measurements: [VM source layout](internal/vm-source-layout.md).

- **An opt-in LTO build of the AOT runtime.** `diamond build` links the runtime at `-O2` without
  LTO. Measured on five `bench/` programs against that: `-O2` with LTO is about 2% slower, and
  `-O3` with LTO about 3% faster (instructions and cycles both), but the LTO link recompiles the
  runtime for every app (about 38 s per `diamond build` instead of 0.2 s). That only makes sense
  as an opt-in (a flag or variable), and the prebuilt kit would then need a second set of fat
  `-O3` objects the user's compiler can link.

### Register allocation

- **A general liveness-based recycling pass (Stage 3)** -- aspirational, not scheduled.
  Hold off until the narrower stages have more real-world mileage. Design and
  invariants: [register recycling](internal/register-recycling-design.md).

### Compiler representation

The native compiler emits bytecode directly and intentionally does not retain a general
AST. A stable intermediate representation could help optimization, tooling, or
incremental compilation, but would add substantial complexity.

Revisit only when at least one concrete consumer can state the required invariants and
demonstrate that current bytecode/source metadata is insufficient.

## Language directions

### Ergonomics without compatibility chasing

Ruby-inspired syntax should remain familiar, but Diamond is not a Ruby compatibility
implementation. New syntax and core methods should satisfy a demonstrated Diamond use
case and have clear typing, dispatch, and error semantics.

Areas worth considering:

- smaller application-driven collection and string conveniences;
- calendar helpers that fit the existing UTC/local/fixed-offset model;
- explicit metaprogramming operations with inspectable behavior;
- better ways to express common typed callback and data-shaping patterns.

### Non-x86_64 architecture validation

Current coverage: [portability](portability.md).

- TSan, Redis/external-database integration, and the remaining package suites are not
  validated on Linux arm64. Widen them only as separate measured increments.
- The JIT is x86-64-only (see "Native-code execution").

### Stable public boundaries

The language, bytecode format, embedding API, and package conventions are still
unstable. Versioning should follow proven external consumers rather than freezing
experimental interfaces prematurely.

A first stability pass should cover:

- source compatibility promises;
- bytecode versioning beyond "same build only" -- cache and AOT blobs are rejected on
  any compiler change, so there is no cross-version format to promise yet;
- native embedding ownership and error contracts, including the `ProgramBuilder` trust
  surface;
- package layout and lockfile compatibility.

## Tooling and distribution

### Self-hosted frontend

The Diamond-written lexer and compiler are useful differential oracles and can bootstrap
through themselves. The native C compiler remains the production frontend.

Keep the self-hosted implementation at practical parity where it protects the language,
but do not duplicate every native optimization unless it advances a specific bootstrap or
language-design goal.

### Fewer languages to build and test

Getting Diamond built, tested and deployed should not need a second scripting language on
the machine, and the build and test dependency set should stay a C compiler, make and a POSIX
shell.

- Treat any new `python3` call in a build, test or tooling path as a regression.
- Node.js (`tests/wasm_repl_test.mjs`, the WebAssembly check) is the one other exception; keep it
  confined to the optional wasm target.

### Portability

Current coverage and the full inventory of checked platform assumptions:
[portability](portability.md).

- **OpenBSD** -- blocked on a real gap: no `<ucontext.h>`, a Fiber-implementation
  limitation, not an unwritten CI job.
- **Debian** -- untested; Ubuntu is the glibc target CI runs.
- **Widening the narrower jobs** (musl, FreeBSD, macOS, arm64) to sanitizers, other
  compilers or package suites needs that coverage checked first, not just enabled.

## Explicitly deferred

- Ruby compatibility as a goal;
- named IANA timezone parsing bundled into the VM;
- shared mutable heaps between OS threads;
- free-form runtime source evaluation (`ClassName.compile_method` compiles a source
  string into one capture-free method for `define_method` and is not a general `eval`);
- a JIT without representative profiling evidence;
- portability claims without continuous testing on the claimed platform;
- runtime class *synthesis* (decided 2026-09): a class's identity is a `uint8_t
  class_index` sharing the byte space the static type system uses, so new classes at
  runtime would need a second, untyped object-model tier. Not worth it without a
  type-tag redesign no concrete use case justifies; see
  [design](internal/design.md)'s `DIAMOND_VALUE_CLASS` section.

## Completion policy

When roadmap work lands:

1. document the end-user behavior in the appropriate guide;
2. record the capability as a concise changelog milestone;
3. keep detailed measurements or rationale in a focused design/benchmark
   document when they remain useful;
4. remove the completed item from this roadmap -- do not leave a description of
   what was done, measured or found behind.
