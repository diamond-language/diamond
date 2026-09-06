# Changelog

Diamond is currently pre-release. This file records user-visible capability
milestones rather than every implementation step; the Git history remains the
authoritative fine-grained record.

## Unreleased

### Fixes

- Fixed a stack-overflow crash (SIGSEGV) in every `DiamondSourceBundle`
  consumer -- the CLI, REPL, and every `lsp/` handler -- on an `-O0`
  debug build, reproducible at zero `require` depth (`make test-lsp`
  segfaulted outright on its first hover request). Root cause:
  `DiamondSourceBundle.segments`, a ~1.6MB fixed array, was embedded
  directly in a struct several call sites declare as a plain stack
  local, sometimes two deep in one call chain. Heap-allocated now (`src/
  loader.c`/`src/loader.h`), the same fix already applied to `Loader`'s
  own arrays for the identical reason.
- `UDPSocket#receive` now accepts `0` (an immediate, real datagram-
  discarding receive, matching `recvfrom`'s own semantics) instead of
  raising `TypeError`, aligning it with `File#read`/`Socket#read`/
  `TLSSocket#read`/`Process::Stream#read`, which all already accepted
  `0`. Found by a native-API consistency audit (docs/roadmap.md); no
  other inconsistency in arity, type, range, or closed-resource checks
  was found across the audited native surface.

### Tooling

- Validated Diamond against a genuinely different libc (Alpine/musl, not
  just a different distro) for the first time, and wired it into CI as a
  continuous third target (`test-musl` job, `.github/workflows/ci.yml`):
  1283/1285 corpus cases pass. Fixed four real issues found along the way
  (a musl feature-test-macro gap needing an explicit
  `#define _DEFAULT_SOURCE` in 15 more `.c` files, a missing
  `ucontext.h` *implementation* on Alpine specifically -- resolved by
  linking `libucontext` -- an unassignable global `stdout` on musl in the
  REPL's own output-capture code, fixed with `dup2` instead, and a
  `-fPIE`/`-pie` default mismatch in Alpine's gcc, fixed by making
  `-fPIE` explicit in `Makefile`'s `CFLAGS_COMMON` for every platform,
  plus a new `LDLIBS_EXTRA` Makefile variable so the musl-only
  `-lucontext` link requirement doesn't touch any other platform's own
  link line). The two remaining failures (`bcrypt.di`,
  `active_record_secure_password.di`, both excluded from the CI run
  itself) are the same real, documented libc limitation, not a bug:
  musl's own `crypt_r` has no bcrypt algorithm at all, so `BCrypt.hash`
  now returns a clear runtime error there instead of failing to compile,
  and `BCrypt.verify` already degraded safely (always reports no match)
  without any change needed. The `test-musl` job is deliberately narrower
  than the glibc `test-all` matrix (GCC only, no sanitizer/tsan builds,
  no package-specific tests) -- those haven't actually been checked
  against musl either. Full inventory of every OS/libc/kernel/toolchain
  assumption Diamond's native implementation depends on, what's verified
  where: new docs/portability.md. See docs/roadmap.md's "Portability".
- Added `DIAMOND_TRACE_STARTUP`, reporting the load/compile/run time split
  for a process on stderr; used to measure prelude-compilation cost for
  docs/roadmap.md's startup-time investigation.
- Added `textDocument/references` to the Language Server: finds every
  workspace occurrence (call/access site, type position, or declaration
  header) of a top-level function/class/module/interface name, walking
  the workspace the same way `workspace/symbol` does. Conservative by
  construction -- a candidate is dropped unless it sits in a resolvable
  position and no lexical local of the same name shadows it there. See
  docs/lsp.md.

### Performance

- Added `diamond_compile_incremental` (`src/compiler.c`), a native
  embedding entry point that compiles source against an already-compiled
  `DiamondProgram` template (e.g. the prelude, compiled once) instead of
  re-lexing/re-parsing the template's own source text on every call.
  `tests/run_cases.c` (the 1285-case batch test corpus runner) is the
  first consumer: compiles the prelude once per process instead of once
  per case, cutting the runner's own wall-clock time ~25% (measured:
  31.0s -> 23.3s).
- Fixed `seed_program_from_template` and `diamond_compile_impl`'s own
  post-discovery tail (`src/compiler.c`) copying entire fixed-size
  `classes`/`interfaces`/`modules` arrays (~14MB combined) on every
  `diamond_compile`/`diamond_compile_incremental` call regardless of how
  many slots were actually in use -- now copies only each table's own
  used-count prefix. No behavior change, unconditionally cheaper.
- Cut `diamond` CLI cold-start compile time roughly in half for the common
  case (~23-24ms -> ~11.5-13ms, release build) via a build-time embedded,
  pre-compiled prelude snapshot (`src/compiled_prelude.{h,c}`, `tools/
  gen_compiled_prelude.c`, `src/compiled_prelude_data.c`), wired into
  `diamond_run_source` (`src/run_source.c`): the common (non-JSON) case now
  skips lexing/parsing/codegening the prelude's own source on every
  invocation. A program needing JSON still falls back to a live compile,
  unchanged. Getting this to actually pay off (a first attempt was a net
  *loss*) also fixed two real, independent problems: `diamond_function_copy`
  collapsed from 6 small `malloc`s per function to 1 (`DiamondFunction.
  owns_combined_buffer`, `src/vm.h`), and `diamond_program_init`'s full
  ~14.2MB `memset` (paid twice per compile call, ~6.3ms each, regardless of
  source size) skipped wherever it's provably redundant --
  `diamond_program_init_fresh`, used for `diamond_compile_impl`'s own
  always-fresh `discovery` program, Thread.new's own program clone
  (`clone_program_from_chunk`), and `compile_method`/`define_method`'s own
  backing store (`allocate_program_builder`) -- which alone cuts ~35% off
  *every* `diamond_compile`/`diamond_compile_incremental` call in the whole
  system, not just this feature.
- Cut `diamond` CLI cold-start compile time further still: the embedded-
  template case from ~11.5-13ms to ~6.6-7.3ms, and the JSON/fallback path
  (and every other `diamond_compile` caller) to ~11.5-12.9ms, by removing
  the *other* half of `diamond_program_init`'s own memset -- the one still
  paid in full for the caller-supplied `program` (left alone in the
  previous entry above, since that struct's zero state isn't guaranteed
  the way a fresh `calloc` is: `tests/run_cases.c`'s own batch loop reuses
  one `DiamondProgram` across 1285+ calls). Doing this safely meant
  auditing every class/module/interface creation site
  (`compile_class`/`compile_module`/`compile_interface`, `src/compiler.c`)
  to confirm each one explicitly resets its own newly-claimed slot's
  fields, rather than relying on the slot already being zero -- found and
  fixed the exact gap this risked: each one's "genuinely new slot" branch
  set only a couple of fields, leaving `methods`/`fields`/`class_
  variables`/every count to leak a *previous* compile's own leftover data
  into a reused `DiamondProgram`. `diamond_program_init_fresh` needed the
  identical fix for the built-in exception classes themselves (`class
  Exception ... end` reopening one to add a method is ordinary, allowed
  monkey-patching, so their own tables can carry stale state too). With
  every creation site self-sufficient, `diamond_program_init` now skips
  memset-ing `classes`/`interfaces`/`modules` (~14.2MB together) entirely,
  doing two small `offsetof`-bounded memsets around them instead. Verified
  against the full 1428-case corpus (`tests/run_cases.c`'s own reused-
  program batch loop is exactly the scenario this needed to get right),
  three dedicated standalone reuse-safety probes built clean under
  ASan+UBSan, and a musl (Alpine) rebuild.
- `JSON.parse` is native now (`String#parse_json`, `src/vm.c`), replacing
  `lib/core/json_codec.di`'s pure-Diamond recursive-descent implementation
  -- roughly 100x faster on realistic payloads (~3.3ms/MiB measured
  against a 13.75MB synthetic corpus, versus the ~330ms/MB the version
  this replaces measured against a real TinyStories shard). Same grammar,
  same JSONError-on-malformed-input contract, same result shapes
  (String/Int/Float/Bool/nil/Array/Hash) -- verified against every
  existing `tests/cases/json_parse_*.di` case. `JSON.stringify` is
  unchanged (no comparable performance problem was ever measured for it).
  `JSONError` itself moved from a `lib/core/json_codec.di` class
  declaration to a VM builtin, so native code can raise it the same way
  every other native error class already does.

### Language

- Added `ClassName.compile_method(name, params, body_source, bound_values)`,
  compiling a method body from a source string at runtime into a `Callable`
  for `define_method` to install -- the missing piece behind
  `active_record`'s `has_many`-style associative helpers. Runs the source
  through the ordinary compiler into an isolated throwaway program (field
  references are validated against the target class's existing fields;
  `bound_values` threads in already-evaluated values the snippet has no way
  to name). See docs/classes-and-modules.md.
- Added `Tensor`, a native dense row-major `Float` matrix (`Tensor.zeros`/
  `.from_array`/`.random`, `#rows`/`#cols`/`#get`/`#set`/`#matmul`/
  `#transpose`/`#to_a`) with a threaded, k-blocked `#matmul` -- extracted
  from broader from-scratch-transformer experimentation as the
  general-purpose numeric-computing piece of that work, independent of it.
  Deliberately a narrow prototype (no broadcasting, non-2D shapes,
  in-place ops, or autodiff), built to measure real `matmul` throughput
  against boxed `DiamondValue` `Array`s before committing to a fuller
  tensor API. See docs/collections.md.
- Added `exp(x)`/`log(x)`/`tanh(x)` alongside the existing `sqrt`/`sin`/
  `cos`/`tan`/`pow` native math functions.
- Added `File.directory?(path) -> Bool`.
- Added visibility-safe `public_send` runtime-name dispatch for native and
  user-defined receivers, with String/Symbol names, argument forwarding,
  inheritance, overrides, variadics, and `method_missing`; private and
  protected targets remain inaccessible.

### Documentation

- Reorganized the monolithic syntax and native-service references into
  navigable guides for core syntax, callables, classes and modules, types and
  errors, collections, runtime features, local I/O, networking, databases,
  time, and processes.
- Reworked Fiber and Thread documentation around end-user behavior and moved
  VM, GC, copying, and lifecycle details into a focused concurrency internals
  document.

### Time and calendar

- Added GC-managed wall-clock `Time` values backed by fractional Unix epochs.
- Added local, explicit UTC, and immutable per-value fixed-offset display modes
  without mutating process-global timezone state.
- Added `Time.now()`, `Time.utc_now()`, `Time.at()`, strict UTC/local/fixed
  calendar constructors, and strict ISO-8601 parsing for `Z` and numeric
  offsets.
- Added calendar fields, libc-backed `strftime`, canonical round-trippable
  `iso8601`, epoch conversion, zone conversion, offset inspection, and default
  string formatting.
- Added elapsed-second arithmetic, subtraction between times, hashing, and
  comparisons by absolute instant across different display zones.
- Added numeric duration units from seconds through weeks and Rails-style
  `ago`/`from_now` helpers.
- Added DST-aware calendar movement by days, weeks, months, and years, including
  end-of-month clamping and preservation of wall-clock time, fractions, and
  display mode.
- Added day, Monday-based week, month, quarter, and year boundaries with
  final-microsecond end semantics.
- Added today/yesterday/tomorrow, past/future, same-day, and weekday/weekend
  predicates evaluated with explicit zone semantics.
- Added constant-time next/previous weekday navigation with optional
  business-day counts.
- Kept named IANA zones deliberately out of scope; the process-local zone and
  explicit UTC offsets are the supported timezone model.

### Native services

- Added reusable JSON database configuration for SQLite3, PostgreSQL, MariaDB,
  and MySQL connections, including named environments and environment-backed
  passwords.
- Added a database benchmark comparing in-process SQLite with resource-limited
  Podman PostgreSQL, MariaDB, and Oracle MySQL services using identical
  driver-level read and update workloads.
- Added asynchronous `Process.spawn` handles with polling, stream reads,
  termination, and wait support.
- Expanded HTTP/TLS support with HTTPS clients, redirects, chunked transfer,
  gzip, authentication, JSON/multipart requests, sessions, ALPN, and TLS
  session resumption.
- Added Base64 and gzip primitives.

## 0.2.0 development milestones

### Language

- Built a Ruby-inspired expression language with functions, closures, blocks,
  defaults, keyword and spread arguments, recursion, destructuring, ranges,
  pattern-oriented `case`, and expression-valued control flow.
- Added classes, inheritance, modules, namespaces, singleton methods,
  visibility, generated attributes, aliases, operator methods, structural
  interfaces, and controlled runtime method definition/replacement.
- Added optional annotations, unions, generics, typed collection and callable
  contracts, flow narrowing, and runtime structural checks.
- Added arbitrary-precision integer promotion, float arithmetic, symbols,
  strings, arrays, hashes, regexps, and value-aware equality/hashing.
- Added rescuable VM failures, typed rescue filters, `else`, `ensure`, `retry`,
  causal exception chains, and source-mapped backtraces.
- Added relative `require`, explicit package `require_cut`, canonical load-once
  behavior, cycle detection, and imported-file source maps.
- Added `ARGV`, `ENV`, debugger breakpoints, interpolation, formatting, and an
  accumulating interactive REPL.

### Core library

- Added a Diamond-written embedded prelude shared by the CLI, REPL, and LSP.
- Added Enumerable and Comparable behavior plus collection mapping, filtering,
  reduction, grouping, sorting, flattening, zipping, tallying, extrema, and
  predicate helpers.
- Added StringBuilder, string transformation/search helpers, numeric helpers,
  ranges, JSON parsing/serialization, and a Minitest-style test library.
- Added bcrypt, secure random bytes, digest/HMAC operations, constant-time HMAC
  verification, AES-256-GCM encryption, Base64, and gzip facilities.

### Runtime and memory management

- Built a C23 register-bytecode VM with validated bytecode, disassembly,
  source-mapped diagnostics, runtime stack traces, and fixed resource guards.
- Added polymorphic method/field caches, stable runtime object shapes,
  monomorphic call-site rewrites, opcode counters, and opt-in integer
  quickening.
- Added generational mark/sweep collection with young/old generations,
  remembered sets, card marking for arrays/hashes, promotion, stress modes, and
  separate minor/major tracing metrics.
- Added cooperative fibers and OS threads with isolated VMs/heaps, copied
  cross-thread values, thread-safe lifecycle handling, and scheduler coverage.
- Added compiler and bytecode fuzz targets, sanitizer/TSan builds, burn-in and
  GC benchmarks, and a large executable regression corpus.

### I/O, networking, databases, and processes

- Added stdin/stdout, files, path operations, nonblocking I/O, polling, TCP,
  UDP, signals, and OpenSSL-backed TLS client/server primitives.
- Added SQLite3 connections and prepared statements with typed binds/results,
  safe close behavior, change counts, and insert identifiers.
- Added PostgreSQL and MySQL connections with parameterized execution and
  querying, verified against live servers.
- Added synchronous subprocess execution with argv isolation and captured
  stdout/stderr/exit status, followed by asynchronous process handles.
- Added monotonic timing and the full wall-clock/calendar surface summarized in
  the Unreleased section.

### Tooling and self-hosting

- Added a Language Server with diagnostics, completion, hover, definitions,
  document/workspace symbols, source mapping, and conservative receiver-aware
  resolution for known classes and unions.
- Added a VS Code extension with syntax highlighting and LSP integration.
- Added REPL history, multiline accumulation, editing, and completion powered
  by the same completion engine as the LSP.
- Added a Diamond-written lexer/compiler, native/self-hosted differential
  suites, and self-compile/self-run bootstrap checks.
- Added `facet`, a git-pinned package installer with lockfiles and explicit
  package loading.

### Packages

- Added HTTP client/server utilities, Rack-style middleware, Gremlin serving,
  route handling, multipart support, cookies and encrypted/signed sessions,
  logging, and log viewing.
- Added Arel-style relational algebra with expressions, joins, subqueries,
  CTEs, set operations, writes, prepared-statement reuse, and SQLite,
  PostgreSQL, MariaDB, and MySQL visitors verified against their engines.
- Added an explicit ActiveRecord layer with repositories, models,
  associations, eager loading, validations, optimistic locking, batches,
  migrations, instrumentation, and nested transactions/savepoints.
- Added GraphQL and GraphSQL packages with schema execution, lookahead-driven
  projection, batching, and persistence integration.
- Added authentication, discussion, social, karma, and application-support
  packages used by repository examples and applications.

### Applications and examples

- Added library, guestbook, and project-board examples showing persistence,
  HTTP/Rack composition, templates, environment-isolated databases, logging,
  and smoke tests.
- Added `applications/pheint.dia`, a GraphQL game/leaderboard API with accounts,
  player profiles, sessions, authentication, rankings, pagination, settings,
  environment configuration, audit logging, and seeded data.
- Added `applications/skindicate.dia`, an application with authentication,
  moderation/administration workflows, persistence, views, and smoke coverage.

## 0.1.0 foundation

- Introduced the Diamond source language, lexer, Pratt compiler, register
  bytecode, VM, managed object model, closures, classes, exceptions, and core
  scalar/collection values.
- Established Linux/GCC as the initial development target and added Make-based
  debug, release, test, sanitizer, benchmark, and fuzz workflows.

## Maintenance policy

- Record notable user-visible additions, removals, compatibility changes, and
  fixes here.
- Keep implementation investigations and benchmark detail in focused design or
  benchmark documents.
- Remove completed work from the roadmap once documentation and changelog notes
  are in place.
