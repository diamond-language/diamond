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

### Debugger follow-ups

Current behavior: [debugging](debugging.md).

- **Throttling the live-breakpoint/step poll** -- `DIAMOND_OP_BREAKPOINT_CHECK`
  does a non-blocking `poll()` on the control fd at every statement. Nothing has
  shown a need to throttle it; if that changes, check only every Nth hit via a
  counter mask, the shape `DIAMOND_RESOURCE_LIMIT_CLOCK_CHECK_MASK` already uses.
- **`supportsStepInTargetsRequest`** -- choosing which call to step into on a line
  with more than one (e.g. `f(g())`).

### `struct` declarations

Current behavior: [classes and modules](classes-and-modules.md).

- **A superclass, or being reopened** -- no obviously correct semantics for
  regenerating `==`/`to_s`/`initialize` against an inherited or changed field list.
  Revisit only with a concrete design.
- **Exact-class `==`** -- the generated `==` accepts any subclass of the struct's
  class, so a subclass with extra fields compares equal on the shared ones. Narrow
  and known; fix only with a driving need.

### freeze / frozen?

Current behavior: [classes and modules](classes-and-modules.md).

- **Frozen `Array`/`Hash` literals** -- freezing at the point a literal is written,
  instead of a `.freeze()` at every construction site. `String` is already
  immutable, so this only matters for collections. Needs a concrete use case.

### Tail-call optimization

Current behavior: [callables](callables.md).

- **Mutual and cross-function tail calls** -- a tail call to a different known
  function needs a different register count, chunk and constant pool, which turns
  "reuse the running frame" into swapping most of `run_chunk`'s locals mid-loop, in
  the most performance-audited code in the project. More risk, broader value.
- **A generic function's own self-tail-call** -- type-variable bindings are derived
  at entry from the call's arguments; redoing that for an in-place loop is unsolved.
- **A variadic function's own self-tail-call** -- overflow arguments are tracked
  against the original call's `arguments`/`argument_count`, which an in-place next
  iteration cannot update without a new mechanism.
- **Reporting that a call was optimized** -- `--dump-bytecode` shows `TAIL_CALL`,
  but no `DIAMOND_TRACE_*` variable reports it at run time.

### Case/when exhaustiveness checking

Current behavior: [core syntax](core-syntax.md).

- **A sealed base in `when` covering its subclasses as a group** -- `when Shape`
  does not count as covering `Circle` and `Square`, even though `Shape` is the sealed
  base both extend. Each direct subclass needs its own `when`. Making this work is a
  third coverage rule, not a reuse of the exact-member match.

### Channel

Current behavior: [threads](threads.md).

- **Send-side select** -- waiting to `send` on whichever of several channels has room.
- **Unbounded or rendezvous channels** -- `Channel.new(0)`-style handoff, or no
  capacity limit. A bound keeps memory predictable and gives `send` backpressure, so
  this needs a concrete use case.

### `Supervisor`

Current behavior: [threads](threads.md).

- **Restart intensity and backoff** -- a policy such as "give up after N crashes in M
  seconds", instead of a fixed 20 ms delay with no cap.
- **Cancel-on-timeout** -- `Supervisor.stop` still requires workers to cooperate.
  Blocking TLS and SQL interruption and implicit cancellation checkpoints are open
  (see [cancellation](cancellation.md)).
- **Cross-thread-transferable supervisor handles** -- a `Supervisor` cannot cross a
  `Thread.new`/`Channel` boundary, and a supervised child cannot reference its parent
  by design. Needs a concrete use case.

### `diamond build`

Current behavior: [deployment](deployment.md).

- **Cross-compilation** -- needs a real story for cross-linking OpenSSL, SQLite3,
  `libpq` and MariaDB for the target, not just a compiler flag.
- **Static linking** -- a hermetic binary with no dynamic dependency on those
  libraries or zlib/`libcrypt`. `diamond` itself does not link statically, so doing
  it for `diamond build` alone would be new, unproven build-system work.

### Sandbox mode

Current behavior: [sandbox](sandbox.md).

- **A configurable grace allowance** -- the cleanup allowance after a budget trips
  (1,000,000 instructions, 1,000 ms, a quarter of the memory budget) is fixed. Needs a
  program whose rescue clause legitimately outgrows it, or one that wants it tighter.
- **Path and host allow-listing** -- capabilities are whole categories; finer
  matching needs a policy format, not more environment variables.
- **Restricting `Thread.new`/`Supervisor.add_child`** -- bounding the OS threads a
  sandboxed program can spawn (today the process-wide 64-thread cap). A
  resource-exhaustion concern; needs a concrete abuse case.
- **Restricting `Signal.trap`** -- a process-wide side effect, distinct from opening
  resources.

### Bytecode caching

Current behavior: [caching](caching.md).

- **A `diamond cache clear`-style command** -- today a stale `.dic` is discarded by
  deleting it or editing the source. Needs someone hitting that friction.
- **Moving the cache out from next to the source** -- a read-only deployment (a
  container image, a system-wide install) cannot write a sibling `.dic` and silently
  never caches. `XDG_CACHE_HOME`-style storage would fix it but needs a different cache
  key, since two `app.di` files in different directories could no longer be told apart
  by path.
- **Caching `diamond -e`** -- excluded for lack of an on-disk identity; a
  content-addressed cache in a dedicated directory could cover it, as the point above.

### Improve receiver-aware tooling

Current behavior: [language server](lsp.md). The language server recompiles complete
documents and loses precision across some dependency and dynamic-flow boundaries.

- Improve receiver facts across imported files where they still lose precision:
  element graphs of collections built from inferred call results, methods whose
  result varies, unrepresentable dynamic results, and nullable `Hash` indexing
  remain unresolved.
- Explore incremental compilation only after the compiler has a reusable unit boundary
  that makes incremental synchronization worthwhile.
- Keep editor results conservative when a receiver cannot be proven.

### LSP formatting

Current behavior: [language server](lsp.md). The formatter normalizes indentation and
trailing whitespace; the native compiler keeps no AST to reflow against.

- **Operator and comma spacing** -- needs its own token-adjacency heuristic.
- **Blank-line collapsing** -- a cosmetic judgment call with no single correct answer.
- **`textDocument/rangeFormatting`** -- the depth tracking would need a starting depth
  handed in from outside the selection.

### Harden the end-user runtime surface

The native service layer is broad enough to build real applications. Work here should
favor consistency, portability, and failure behavior over adding unrelated primitives.
Results and test commands belong in the topic guides and test scripts.

- Continue stress-GC, sanitizer, thread, socket, TLS, subprocess, and database coverage;
  re-run focused audits as new native services land.
- Extend the bytecode execution fuzzer (`make fuzz`) to the opcodes it still rejects
  because the sandbox does not gate them: `IO_POLL`, `TLS_START_HANDSHAKE`, `SIGNAL_TRAP`
  and `THREAD_NEW`.
- Document platform-dependent behavior explicitly.
- Preserve thread safety: per-value state is preferred to mutation of process-global
  settings.

### Grow packages from application needs

New package work should be driven by a concrete application or interoperability need.

- Deepen ActiveRecord and Arel only where applications expose a missing query,
  association, migration, or dialect feature.
- Add return-type annotations (`-> Type`) to the Arel and ActiveRecord methods still
  without one: the nullable accessors (`Query#limit_value`, `Query#offset_value`,
  `Query#table_alias`, `Query#source_query`), `Query#count`, `Relation#first`/`#count`,
  `Repository#find`/`#create`/`#update`/`#delete`, and the rest of `Model`. The JIT never
  trusts an unannotated return, and each batch measured so far paid off on Skindicate's
  batch loaders (see [JIT design](internal/jit-design.md#where-the-next-win-is)).
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
