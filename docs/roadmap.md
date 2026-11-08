# d6 roadmap

This roadmap tracks possible work. Completed work belongs in the
[changelog](../CHANGELOG.md), current behavior in the topic guides, and
implementation rationale in design documents. A section here says what is not
done yet; when something lands, delete it (see "Completion policy").

d6 is a research language. Priorities can change when measurements expose
a more valuable runtime, language, or tooling question.

Broader reliability work waits for usage beyond the current two users; existing verification
remains required. Package development prioritizes Mjolnir, with ActiveRecord in the
background. Further JIT work is optional and measurement-driven.

## Candidate work

These are possible next steps, not commitments. Start with a measured need or
a concrete application gap.

### Rebrand Diamond as d6

The user-facing name becomes **d6**, which remains a reference to Diamond. The name
leans into the dice: rewriting in d6 is a saving throw.

- **Logo:** the site still uses the gem mark; a dice mark is the owner's call.
- **Brand capitalization (low priority):** the brand is **D6**, capitalized: the site's headings
  and page titles, the README title, the logo wordmark and the GitHub release titles
  (`packaging/publish_release.sh` still titles them "Diamond X.Y.Z"). Commands, file names and
  running text in the guides stay lowercase `d6`.
- **GitHub:** the org becomes `roll1d6` or `rolld6` (not yet chosen). Rename it last.
- **Domain:** to be chosen later. `cuts.dilang.tech` stays as the registry address.
- **Unchanged:** the **cut** and **facet** names, and `DIAMOND_` constants, environment
  variables and internal identifiers. Avoid plumbing changes whose only purpose would be
  to replace the codename.

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

### `d6 build`

Current behavior: [deployment](deployment.md).

- **Targets beyond 64-bit little-endian** -- a 32-bit or big-endian target needs a
  portable program image rather than the current struct dump.
- **MySQL in static kits** -- a static musl kit leaves the MariaDB client out. The portable
  `d6` already builds Connector/C statically (`tools/build_portable_inside.sh`); the kit
  could reuse that recipe.
- **Static output for glibc and non-Linux targets** -- only musl/Linux is supported.

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
  as editor-only facts. Direct nullable
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
- Add another SQL dialect only with a live server for verification.
- Improve package documentation, examples, and compatibility tests.
- Avoid framework magic that hides database access or weakens d6's type and error
  contracts.

## Runtime research

### Bound pause time further

Collections are stop-the-world within each isolated VM.

- Measure pause distributions on long-lived application workloads.
- Tune promotion and collection thresholds using evidence.
- Investigate incremental marking only if pause measurements justify the added barrier
  and state-machine complexity.

### Close the benchmark gaps to Ruby

The local `fistfight` suite pairs d6 and Ruby programs (21 pairs). On 0.29.0-dev the d6
interpreter beats the Ruby interpreter on 20 of them and ties on fib (1.03x). The gaps left
are against YJIT, on call-heavy and floating-point object code: fib 2.7x, poly 2.0x, nbody
2.0x, trees 1.9x, sieve 1.25x, loop 1.2x, iterate 1.1x (best d6 over best Ruby). d6's own JIT
(opt-in) barely moves those: fib 195 to 161 ms, and trees, nbody, poly and objects not at all.

- **JIT coverage** -- the JIT speeds up `loop`, `mandelbrot` and `sieve` (simple numeric
  loops) but not calls, field access or Float objects. Traced on 0.29.0-dev:
  - Block bodies run as native code now, but every block call still goes through C
    (`block_call_run` and a register copy) rather than a native call from the loop.
  - Functions the JIT still turns away, across the test cases, benchmarks and examples:
    variadics (22 files) and functions a block's `break` lands in (12). Then a tail of opcodes:
    `SQLITE3_OPEN` (39), `JSON_STRINGIFY` (25), `CASE_MATCH` (25), `THREAD_NEW` (15),
    `GET_IVAR_NAME` (14), `REGEXP_NEW` (13), `TO_INT`, `BLOCK_BREAK`/`BLOCK_RETURN` (11 each).
    Opcodes with an out-of-line interpreter body can join `diamond_jit_outlined` with just a
    decoder entry each.
  - Polymorphic and untyped call sites pay the full dispatch per call (poly's
    `shapes[i].area()`); a per-site inline cache checked in native code would cut that.
  - A compiled function's call to itself is native now (fib 121 ms against YJIT's 60), but
    calls to other functions and methods still go through C trampolines: in trees,
    `diamond_jit_invoke_instance` and `diamond_jit_new_instance` lead. Gains need native
    calls between compiled functions, and inline field reads and writes.
  - Compiled bodies keep every value in memory and re-check types the caller already proved
    (fib's `n < 2` reloads and re-tests the tag of a parameter the entry check just
    confirmed). Most of fib's remaining gap to YJIT is register allocation and type
    propagation inside a body.
- **Call machinery** -- about 300 of a fib call's ~450 instructions are call setup (prologue,
  register zeroing, CALL and RETURN), spread thinly, and frame size is not the cost. A
  VM-stack frame design is the remaining large lever; inline frames were tried and lost cycles
  to register pressure and store-forwarding stalls, so a new design has to avoid both.
- **Code layout** -- even with profile-guided builds, `run_chunk` performance moves with code
  placement: answering `Regexp#match`/`match?` inside it took the regex benchmark 18% faster
  but slowed blocks 3.5-4.6% at identical instruction counts, so it was dropped. Control over
  the hot loop's layout would unlock fast paths like that one.
- Smaller measured levers, no longer gaps: a raise formats a backtrace String per frame
  (capture frames, format when read); string interpolation allocates once per piece (a fused
  opcode needs compiler, JIT and bytecode-cache changes together).

Compare instruction counts (`perf stat -e instructions:u`) on PGO release builds, and keep a
change only if cycles agree.

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

Ruby-inspired syntax should remain familiar, but d6 is not a Ruby compatibility
implementation. New syntax and core methods should satisfy a demonstrated d6 use
case and have clear typing, dispatch, and error semantics.

Areas worth considering:

- smaller application-driven collection and string conveniences;
- calendar helpers that fit the existing UTC/local/fixed-offset model;
- explicit metaprogramming operations with inspectable behavior;
- better ways to express common typed callback and data-shaping patterns.

### Non-x86_64 architecture validation

Current coverage: [portability](portability.md).

- Linux arm64 runs only GCC on Ubuntu: Clang, Fedora and the fuzz targets are not run there.
  Widen them only as separate measured increments.
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

The d6-written lexer and compiler are useful differential oracles and can bootstrap
through themselves. The native C compiler remains the production frontend.

Keep the self-hosted implementation at practical parity where it protects the language,
but do not duplicate every native optimization unless it advances a specific bootstrap or
language-design goal.

Every `tests/cases` program compiles and runs through it (`tools/self_host_survey.sh`).

- **Keep parity checked** -- the survey runs by hand. A scheduled CI job (not per pull
  request: it compiles every case through the self-hosted compiler) would catch a language
  change that leaves it behind.
- **Bytecode differential** -- compare the bytecode both compilers emit, not just the
  programs' output, to find divergences that happen to print the same thing.

### Fewer languages to build and test

Getting d6 built, tested and deployed should not need a second scripting language on
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

### Cross-distro builds

Current packages: [packaging](../packaging/) builds one `.deb` on Ubuntu 26.04 and one `.rpm`
on Fedora 44, for x86-64 and arm64. Each runs only on the distro release it was built on, or
newer. Every other Linux can use the portable static tarball attached to each release
([deployment](deployment.md#the-portable-d6)).

- **More distros and releases** -- Debian stable, Ubuntu LTS, older Fedora, openSUSE, Arch and
  Alpine. The obstacle is the compiler, not the package format. GCC 15 is needed only for
  `#embed`, in two files (`src/prelude.c`, `src/compiled_prelude_data.c`); a GCC 14 sweep
  rejected nothing else. Generating those arrays when `#embed` is missing would bring the
  floor to GCC 14 (`<stdckdint.h>`), which Debian 13 and Ubuntu 24.04 ship; a
  `__builtin_*_overflow` fallback for `ckd_*` would reach GCC 13. Clang 19 already has
  `#embed`.
- **Portable binary speed** -- the static musl `d6` (`tools/build_portable.sh`) targets
  baseline x86-64 and has no JIT. Measure what the baseline ISA and musl's allocator cost
  against the x86-64-v3 packages; if it is large, try runtime dispatch for the hot paths or a
  faster allocator. The JIT is gated on glibc; check what keeps it off musl.
- **`d6 build` without a compiler** -- with the portable binary as the runtime, `d6 build`
  could append the program image to a copy of itself instead of running `make` and a C
  compiler, so a downloaded `d6` could build standalone executables on any Linux.
- Test each package in a clean container of its target before calling it supported, without
  upgrading the system first (that is how the 0.22 walkthrough found missing dependencies).

## Explicitly deferred

- Ruby compatibility as a goal;
- named IANA timezone parsing bundled into the VM;
- shared mutable heaps between OS threads;
- free-form runtime source evaluation (`ClassName.compile_method` compiles a source
  string into one capture-free method for `define_method` and is not a general `eval`);
- a JIT without representative profiling evidence;
- portability claims without continuous testing on the claimed platform;
- runtime class *synthesis* (decided 2026-09): a class's identity is a 16-bit
  `class_index` inside the type-tag space the static type system uses, from a table sized at
  compile time (`DIAMOND_MAX_CLASSES`), so new classes at runtime would need a second, untyped
  object-model tier. Not worth it without a
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
