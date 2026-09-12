# Changelog

Diamond is currently pre-release. This file records user-visible capability
milestones rather than every implementation step; the Git history remains the
authoritative fine-grained record.

## Unreleased

### Concurrency

- Added `Channel`, a bounded, thread-safe mailbox: `Channel.new(capacity)`,
  `send`/`receive` (blocking), `try_send`/`try_receive` (raise
  `WouldBlockError` instead of waiting), `close`/`closed?`/`size`. Unlike
  `Thread#join`'s own one-shot result handoff, a channel lets threads
  exchange values throughout their whole lifetime. Every payload is still
  deep-copied across the heap boundary exactly the way `Thread.new`
  arguments and `Thread#join` results always have been (`Threads do not
  share mutable objects`, docs/threads.md) -- the channel itself is the one
  new exception, a genuinely shared, refcounted native resource with its
  own private `DiamondVm` existing purely as GC-managed storage for values
  in transit. See docs/threads.md's Channels section and
  docs/internal/concurrency-internals.md for the full design.
- Added `Supervisor`, restart-on-crash structured concurrency:
  `Supervisor.new()`, `add_child(callable, *args)`, `stop`/`join`,
  `restart_count`/`last_error`/`alive?`. An uncaught exception or internal
  VM failure in a supervised worker restarts only that worker
  (`one_for_one`, matching Erlang's own default) with a fresh isolated
  heap per attempt, rather than ending it the way a plain `Thread` would;
  a clean return still ends it for good. Genuine supervision trees need no
  extra mechanism -- a supervised child is just a closure free to create
  and manage its own nested `Supervisor`. One real OS thread per child for
  its whole supervised lifetime, looping internally across restarts rather
  than being recreated per attempt. See docs/threads.md's Supervisors
  section and docs/internal/concurrency-internals.md for the full design.

### Tooling

- Extended receiver-aware call-chain resolution (`textDocument/hover`/
  `definition`/`completion`) to functions whose body uses `return`
  across separate branches with no shared annotation: `compile_return`
  now accumulates every explicit `return value`'s own type into a
  running union, merged into `compile_definition`'s existing inference
  alongside its trailing-expression case rather than replacing it. A
  bare trailing `if`/`case` expression already resolved this way
  (`merge_flow_types` already wrote the union straight onto that
  expression's own destination register); `return`-based branches were
  the real remaining gap, since `body_result` alone can never see a
  value that exited through an earlier `return`. See docs/roadmap.md's
  "Improve receiver-aware tooling".
- Added a v1 step debugger: `diamond-dap` (`dap/`), a real Debug Adapter
  Protocol server giving an editor's own gutter breakpoints the exact
  pause `debugger()`/`breakpoint()` already had, without editing source
  -- a real call stack and locals at the paused frame, via a new
  compile-time breakpoint mechanism (`diamond_compile_with_breakpoints`,
  `DIAMOND_DEBUG_BREAKPOINTS`/`DIAMOND_DEBUG_FD`) that makes zero changes
  to `run_chunk`'s own dispatch loop or the bytecode format. No step-over/
  into/out yet, and changing a breakpoint means restarting the debuggee --
  see docs/debugging.md for the full contract and limitations, and
  docs/roadmap.md's "Step debugger v2" for what's next. `editors/vscode`
  wires this up as a VS Code debugger via `diamond.debugAdapterPath`.
- Added `diamond build SOURCE [-o OUTPUT]`, producing a standalone native
  executable with no separate interpreter, `.di` source file, or
  recompilation step at run time -- reusing the same compiled-program
  serialize/deserialize mechanism the embedded prelude template already
  relied on (`diamond_program_write_compiled`/`_read_compiled`). Fixed a
  latent bug this surfaced in that same mechanism: a deserialized
  program's `DiamondClass.shapes[]` self-referential pointers still
  pointed at the *original* program's `classes[]` array, silently
  corrupting every field access on a user-defined class once that
  original program was gone. `diamond_program_recompute_shapes` now
  fixes those pointers up wherever `classes[]` is populated without a
  following real compile pass. See docs/deployment.md for the CLI
  contract and what it doesn't do (no cross-compilation, no static
  linking, must build from a repo checkout), and docs/roadmap.md's
  "`diamond build`: what's next, if anything" for what's deferred.

### Security

- Added sandbox mode (`diamond --sandbox`/`DIAMOND_SANDBOX=1`): denies every native
  call that opens a real filesystem, network, or subprocess resource --
  `File.open`/`.delete`/`.directory?`/`.expand_path`, `Dir.entries`,
  `TCPSocket.connect`, `TCPServer.listen`, `UDPSocket.bind`/`.open`,
  `TLSSocket.connect`, `TLSServer.listen`, `SQLite3.open`, `PostgreSQL.open`,
  `MySQL.open`, `Process.run`/`.spawn` -- raising a rescuable `SandboxError`
  instead. Checked directly against the real process environment at each gated
  opcode rather than through a per-`DiamondVm` field, so it applies identically to
  the top-level program and anything it spawns (`Thread`, `Supervisor`,
  `ProgramBuilder#run`) with no propagation code to write or forget. Ordinary
  computation and `puts`/`print` output are unaffected. See docs/sandbox.md for
  the full deny list and what's explicitly not covered yet (per-capability
  granularity, resource limits, `Thread`/`Signal.trap` restriction).

## 0.4.0

### I/O, networking, databases, and processes

- Fixed a real thread-safety gap found while auditing native surfaces for
  process-global mutation (docs/roadmap.md's "harden the end-user runtime
  surface" priority): `MySQL.open` relied on `mysql_init`'s own implicit,
  undocumented-as-safe `mysql_library_init` call the first time any thread
  in the process opened a MySQL connection. Diamond threads are independent
  OS threads with isolated heaps (docs/threads.md) that can each reach
  `MySQL.open` before any other thread has, so two threads racing their
  first connection at once could corrupt the client library's own one-time
  setup. `diamond_vm_init` now runs an explicit, `pthread_once`-guarded
  `mysql_library_init` call up front -- once per process, from every VM's
  own init, the same "once per VM, idempotent" shape already used there for
  ignoring `SIGPIPE` -- removing the race instead of relying on the
  implicit path. SQLite's and libpq's own first-connection paths were
  checked too: both document their own init routines as safe under
  concurrent first use, so neither needed the same treatment.
- Fixed `Time#strftime`'s `%z` (and `#to_s`'s own use of it) silently
  printing the wrong UTC offset for a fixed-offset Time on macOS -- found
  by `test-macos-ci`'s own trial CI run. `%z` there had relied on the
  platform's `strftime(3)` honoring a `tm_gmtoff` Diamond hand-patches
  into a `gmtime_r`-produced `struct tm`; glibc trusts that field,
  Darwin's libc doesn't, so every fixed-offset Time printed "+0000"
  regardless of its real offset there. Diamond now substitutes `%z`
  itself before the format string reaches the system implementation for
  a fixed-offset Time, rather than depending on the platform to honor
  it -- fixes the behavior on every platform, not just Darwin. Every
  other directive still delegates straight through to `strftime(3)`; UTC
  and Local Time needed no equivalent change (see docs/time.md).

### Packages

- Added `packages/jobs`: a durable, database-backed background/
  scheduled job queue and worker (`Jobs.enqueue`/`enqueue_in`/
  `enqueue_at`, `Jobs::Worker.run_once`/`run_forever` with retry/
  backoff, and fixed-interval recurring jobs via
  `Jobs.schedule_recurring`). Driven by a concrete need (skindicate.dia's
  ingest scripts and moderation-point allocator); creates no tables of
  its own, matching `ActiveRecord::Migrator`'s own schema-free
  convention. See `packages/jobs/README.md`.

## 0.3.0

### Language

- Anonymous `do ... end` blocks now capture `self` inside an instance or
  singleton method, the same way a named nested `closure` already did.
  Previously `self` inside such a block resolved to garbage (typically
  the block's own first argument) rather than the enclosing method's
  receiver -- a silent correctness bug, not a compile-time error. See
  docs/callables.md.
- Fixed a real Callable-arity bug found while auditing packages/http
  and friends for newer collection idioms: a plain nested `def`
  written directly inside a `def self.x` singleton method (the shape
  `redefine_method`'s own patch-factory idiom relies on) miscounted
  its own arity by one wherever it was used as an ordinary Callable
  value -- passed to `Array#find`, stored in a variable and called,
  etc. -- because the implicit self slot that shape's owner_class
  folds into the underlying function's arity was never subtracted
  back out before comparing against or publishing a `Callable[N]`
  contract. `items.find(matches)` from inside such a method now works
  the same way it already did from a plain function or an ordinary
  instance method.

### Tooling

- Added a standalone semver library (`tools/semver.c`/`tools/semver.h`):
  parsing, precedence ordering, `^`/`~`/comparator range syntax,
  satisfaction checks, and range intersection.
- `facet` dependencies now support a `version` constraint (`^1.2.3`,
  `~1.2.3`, `>=1.0.0 <2.0.0`, an exact `1.2.3`) as an alternative to a
  pinned `tag`/`branch`/`commit`, resolved against a repository's own
  git tags (still no hosted registry). Two requesters constraining the
  same cut intersect instead of hard-conflicting, as long as some
  version satisfies both; `facet.lock` records the resolved tag for
  transparency. See docs/roadmap.md and docs/packages.md.
- Fixed two real, previously-undiscovered bugs in `facet` found while
  building it under a sanitizer for the first time: a crash on the very
  first manifest read (uninitialized memory passed to the compiler,
  pre-existing, unrelated to the version-resolution work above) and a
  memory leak on every manifest/lockfile read (harmless in practice --
  `facet` is short-lived -- but real).

## 0.2.1

### Tooling

- The Language Server now resolves call-chain receivers through
  unannotated single-expression functions/methods (`def make_branch() =
  Branch.new()` used as `make_branch().leaf()`), not just explicitly
  `-> Type`-annotated ones -- inferred the same way an unannotated
  block's own return type already was, kept in a separate compiler field
  from real type-checking so this is a pure, zero-risk tooling
  improvement. See docs/lsp.md and docs/roadmap.md.
- Fixed the self-hosted lexer/parser's `>>` (shift right) support --
  never implemented at all, silently lexing as two `greater` tokens;
  found by the differential test suite, not by auditing.

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
- Added `ClassName.compile_method` (runtime method-body compilation from a
  source string for `define_method` to install -- the missing piece behind
  `active_record`'s associative helpers) and visibility-safe `public_send`
  runtime-name dispatch, with String/Symbol names, argument forwarding,
  inheritance, overrides, variadics, and `method_missing` support.
- Added `Tensor`, a native dense row-major `Float` matrix with a threaded,
  k-blocked `matmul`, plus `exp`/`log`/`tanh` math functions and
  `File.directory?`.

### Core library

- Added a Diamond-written embedded prelude shared by the CLI, REPL, and LSP.
- Added Enumerable and Comparable behavior plus collection mapping, filtering,
  reduction, grouping, sorting, flattening, zipping, tallying, extrema, and
  predicate helpers.
- Added StringBuilder, string transformation/search helpers, numeric helpers,
  ranges, JSON parsing/serialization, and a Minitest-style test library.
  `JSON.parse` moved to a native implementation (~100x faster on realistic
  payloads), same grammar/contract/result shapes; `JSONError` is a VM builtin.
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
- Cut `diamond` CLI cold-start compile time roughly 3-4x (~23-24ms down to
  ~6.6-13ms depending on path) via prelude-template reuse
  (`diamond_compile_incremental`), a build-time embedded pre-compiled prelude
  snapshot, and removing several provably-redundant full-array `memset`s from
  program initialization (~35% additional overhead cut across every compile
  call VM-wide) -- verified against the full 1428-case corpus, dedicated
  reuse-safety probes under ASan+UBSan, and a musl (Alpine) rebuild.
- Fixed a stack-overflow crash (SIGSEGV) in every `DiamondSourceBundle`
  consumer (CLI, REPL, every `lsp/` handler) caused by a ~1.6MB fixed array
  embedded directly in a stack-local struct; heap-allocated now.

### I/O, networking, databases, and processes

- Added stdin/stdout, files, path operations, nonblocking I/O, polling, TCP,
  UDP, signals, and OpenSSL-backed TLS client/server primitives (HTTPS
  clients, redirects, chunked transfer, gzip, authentication, JSON/multipart
  requests, sessions, ALPN, and TLS session resumption).
- Added SQLite3 connections and prepared statements with typed binds/results,
  safe close behavior, change counts, and insert identifiers.
- Added PostgreSQL and MySQL connections with parameterized execution and
  querying, verified against live servers, plus reusable JSON database
  configuration (named environments, environment-backed passwords) and a
  benchmark comparing in-process SQLite against resource-limited containerized
  Postgres/MariaDB/MySQL.
- Added synchronous subprocess execution with argv isolation and captured
  stdout/stderr/exit status, followed by asynchronous `Process.spawn` handles
  with polling, stream reads, termination, and wait support.
- Added monotonic timing and a full wall-clock/calendar `Time` type: local/
  UTC/fixed-offset display modes, `Time.now`/`Time.utc_now`/`Time.at`, strict
  ISO-8601 parsing/formatting, elapsed-time arithmetic and comparisons across
  display zones, duration units with Rails-style `ago`/`from_now`, DST-aware
  calendar movement (days/weeks/months/years, end-of-month clamping),
  day/week/month/quarter/year boundaries, today/yesterday/past/future/weekday
  predicates, and constant-time weekday navigation with business-day counts --
  named IANA zones deliberately out of scope; process-local zone and explicit
  UTC offsets are the supported model.
- Added Base64 and gzip primitives.

### Tooling and self-hosting

- Added a Language Server with diagnostics, completion, hover, definitions,
  document/workspace symbols, source mapping, `textDocument/references`, and
  conservative receiver-aware resolution for known classes and unions.
- Added a VS Code extension with syntax highlighting and LSP integration.
- Added REPL history, multiline accumulation, editing, and completion powered
  by the same completion engine as the LSP.
- Added a Diamond-written lexer/compiler, native/self-hosted differential
  suites, and self-compile/self-run bootstrap checks.
- Added `facet`, a git-pinned package installer with lockfiles and explicit
  package loading.
- Validated Diamond against a genuinely different libc (Alpine/musl) for the
  first time and wired it into CI as a continuous third target (1283/1285
  corpus cases pass; the two known exceptions are a documented musl `crypt_r`
  limitation, not a bug). Full platform/libc/toolchain assumption inventory:
  docs/portability.md.

### Packages

- Added HTTP client/server utilities, Rack-style middleware, Gremlin serving,
  route handling, multipart support, cookies and encrypted/signed sessions
  (plus flash messaging that survives exactly one redirect), logging, and log
  viewing.
- Added Arel-style relational algebra with expressions, joins, subqueries,
  CTEs, set operations, writes, prepared-statement reuse, and SQLite,
  PostgreSQL, MariaDB, and MySQL visitors verified against their engines.
- Added an explicit ActiveRecord layer with repositories, models,
  associations, eager loading, validations, optimistic locking, batches,
  migrations (plus a `rails generate migration`-style file generator),
  instrumentation, and nested transactions/savepoints.
- Added GraphQL and GraphSQL packages with schema execution, lookahead-driven
  projection, batching, and persistence integration.
- Added authentication, discussion, social, karma, and application-support
  packages used by repository examples and applications.
- Added a `div` template runtime helper (escaped `hidden_field_tag`) for
  markup otherwise repeated by hand at every call site.

### Documentation

- Reorganized the monolithic syntax and native-service references into
  navigable guides for core syntax, callables, classes and modules, types and
  errors, collections, runtime features, local I/O, networking, databases,
  time, processes, and portability.
- Reworked Fiber and Thread documentation around end-user behavior, and split
  `docs/` into these public reference guides plus `docs/internal/`
  (design/VM/GC rationale, fuzzing, historical audits) for implementation
  background that isn't needed to write or run Diamond programs.

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
