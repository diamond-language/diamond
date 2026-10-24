# Diamond roadmap

This roadmap tracks possible work. Completed work belongs in the
[changelog](../CHANGELOG.md), current behavior in the topic guides, and
implementation rationale in design documents. A section here says what is not
done yet; when something lands, delete it (see "Completion policy").

Diamond is a research language. Priorities can change when measurements expose
a more valuable runtime, language, or tooling question.

Current focus is 0.19.x: finishing the refactor of oversized source files and
functions, shipped as one or more point releases. 0.20 then focuses on tooling and
documentation: a clear path from installation to a first working app. Broader
reliability work waits for usage beyond the current two users; existing verification
remains required. Package development prioritizes Mjolnir, with ActiveRecord in the
background. Further JIT work is optional and measurement-driven.

## 0.19.x: finish the source refactor

Target: no function over about 200 lines other than `run_chunk`'s dispatch switch,
and no file far past 3,000 lines. Each step is a behavior-preserving move or split,
verified as in [the VM source layout note](internal/vm-source-layout.md) ("How a move
is done"), with release benchmarks treated as a no-regression check.

- **The rest of `run_chunk`** (3,559 lines) -- the remaining larger cases (`TAIL_CALL`,
  132 lines; integer division, modulo and comparison) only where outlining measures
  neutral. The `outlined_*` case helpers could move to their own file.
- **`vm.c`** (10,338 lines) -- continue splitting by subsystem.
- **Compiler and JIT files** -- `compiler_classes.c`, `compiler.c`, `jit.c`,
  `compiler_builtins.c` (2,600-2,700 lines each).
- **Functions over 200 lines** -- `compile_body` (jit.c), `diamond_repl_run`,
  `outlined_tls_connect`, `outlined_invoke_spread`, `channel_invoke_helper`,
  `process_run_with_options`, and the two largest disassembler functions.
- **Call-depth limit** -- `run_chunk`'s frame is now about 40% smaller at `-O3` and
  60% smaller under ASan, so `DIAMOND_MAX_CALL_DEPTH` (92) could be raised. It is
  user-visible and must be recalibrated on every build variant (see its comment in
  `src/vm_internal.h`).

## 0.20: from installation to a first app

- **Getting-started guide** -- one path from installing Diamond through the REPL, a
  first script, a project with a `diamond.cut` and dependencies, tests, and
  `diamond build`/deployment. That material is now spread across the README and
  the topic guides, and `docs/` has no reading order.
- **Project generators** -- a command that creates a runnable, tested skeleton per kind
  of app: command-line tool, Rack web app, TCP/UDP server, and library cut. Each
  generated project should pass its own tests out of the box, checked in CI. Open
  question: `diamond new` or `facet new` (`facet init` today only writes a manifest).
- **`.deb` and `.rpm` packages** -- for Fedora 44 and later and Ubuntu 26.04 and later,
  attached to each release. Releases are tags only today, with no binaries. `make install`
  already honours `DESTDIR` and `PREFIX` and installs `diamond`, `facet`, `diamond-lsp`,
  `diamond-dap` and the AOT kit, but not `lib/`. The binaries link SQLite, libpq,
  MariaDB Connector/C, OpenSSL, libxcrypt and zlib dynamically, so each package needs the
  matching runtime dependencies. Needs a CI job per distribution that builds the
  package, installs it in a clean container, and runs a smoke test.

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

- **`select_send` on rendezvous channels** -- `Channel.new(0)` exists (see threads.md), but
  `Channel.select_send` refuses it: the send-side select returns once a value is placed,
  which cannot promise a receiver has it. Needs a concrete use case. An unbounded
  channel is deliberately not planned: the capacity bound is what keeps memory
  predictable and gives `send` backpressure.

### `Supervisor`

Current behavior: [threads](threads.md).

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
  union methods now retain differing representable Array element graphs through
  direct indexing and copying transforms. Declared Hash lookup locals now retain
  compiler-proven nil guards for editor queries. Representable inferred Hash
  results now carry nullable value graphs through wrappers and explicit guards
  as editor-only facts. Truthiness guards also refine editor receivers while
  preserving Bool alternatives and checked compiler behavior. Short-circuit
  right operands now receive scoped editor proofs, with restoration afterward
  and conservative invalidation after tracked writes. Direct nullable
  indexing and unrepresentable dynamic results remain unresolved.
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

- Grow Mjolnir from application needs; deepen ActiveRecord and Arel only where
  applications expose a missing query, association, migration, or dialect feature.
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
