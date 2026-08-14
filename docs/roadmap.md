# Diamond roadmap

Milestones describe executable capabilities. Research priorities may reorder
future work.

## Completed foundation

### Frontend and bytecode

- Source spans and caret diagnostics.
- Pratt expression compiler with direct register-bytecode emission.
- Debug/release builds and bounds-safe bytecode disassembly.
- Inline (`-e`) and file execution.
- Comments, semicolon separators, digit separators, and trailing commas.

### Dynamic language core

- Integers with checked arithmetic, IEEE-754 floats, booleans, `nil`, and
  strings.
- Locals, assignment, comparisons, expression-valued `if` and `while`.
- Ruby-style truthiness and value-preserving `!`, `&&`, and `||`.
- Explicit `return`, `break`, and `next` with nested control-flow targeting.
- Named functions, positional parameters, recursion, arity checks, and isolated
  register frames.

### Objects

- Classes, constructors, instances, fields, methods, and explicit `self`.
- Single inheritance, overriding, inherited constructors/fields, and lexically
  anchored `super(arguments)`.
- Dynamic receiver dispatch through class metadata.

### Managed collections

- Strings with concatenation and value equality.
- Arrays with literals, printing, indexed reads and writes, and bounds errors.
- Hashes with literals, value-key lookup, missing-key `nil`, insertion,
  replacement, growth, and printing.
- Stop-the-world mark/sweep collection with explicit frame roots and recursive
  tracing of instances and collections.
- Stress collection before every eligible allocation.

### Gradual typing

- Optional parameter and return annotations.
- `Int`, `Float`, `String`, `Bool`, `Nil`, `Array`, `Hash`, and nominal class
  types.
- Subtype-aware class checks and reusable unions of up to eight types.
- Guards on every implicit and explicit typed return path.
- Exact local type facts that remove proven guards and reject proven errors.
- Detailed expected/actual runtime type diagnostics.
- Per-instruction source coordinates in bytecode dumps.
- Source-mapped runtime stack traces across functions and methods.
- First-class nested closures with escaping, shared mutable capture cells across
  recursively nested lexical environments.
- VM-owned four-entry polymorphic caches for dynamic method call sites.
- Class-owned runtime shape chains with lazy instance field transitions.
- Value-carrying `raise` with source-mapped cross-call unwinding.
- Nested `begin`/`rescue` expressions with optional raised-value bindings.
- Primitive and subtype-aware nominal filters on rescue bindings.
- Pipe-separated rescue type sets with up to eight alternatives.
- Shape-guarded polymorphic inline caches for instance field reads and writes.
- Built-in exception hierarchy with rescuable VM runtime failures.
- `ensure` unwinding for normal completion, exceptions, and returns.
- General union annotations for parameters and returns.
- Persistent, recursively checked `Array[Element]` annotations.
- Persistent `Hash[Key, Value]` contracts for entries and mutations.
- C23-embedded Diamond core prelude with initial collection helpers.
- Native collection `length()` with Diamond-written last/fallback/predicate helpers.
- Flow-sensitive generic result facts for array and hash indexing.
- Growable arrays with guarded `push`/`pop` and Diamond iteration helpers.
- Branch-sensitive nil narrowing with conservative local-fact joins.
- Ordered hash iteration primitives with Diamond-written transforms.
- Non-throwing `is` predicates with primitive and nominal union narrowing.
- Runtime `Callable[n]` contracts for Diamond core callbacks.
- Declared `Callable[n, Return]` result contracts and typed array transforms.
- First structural interface, `Sized`, implemented by native containers and
  user classes that provide zero-arity `length`.
- User-declared structural interfaces with implicit name-and-arity conformance
  for native objects and inherited class methods.
- Function-safe structural signature checks with contravariant parameters and
  covariant returns.
- Endless expression-bodied function and method definitions.
- Trailing default arguments for functions, methods, constructors, and
  closures, with supplied-argument tracking in bytecode.
- Double-quoted expression interpolation with scalar and instance conversion.
- Compile-time multi-file `require` with relative paths, extension inference,
  load-once semantics, cycle detection, and dependency-aware diagnostics.
- Inherited `to_s` stringification for instances and cycle-safe recursive
  collection interpolation.
- Scoped generic function type-variable declarations with retained recursive
  metadata and explicit runtime erasure.
- Runtime generic binding from values and callable returns, with persistent
  primitive, nominal, and union substitutions on returned collections.
- Bounded recursive generic binding graphs for nested Array and Hash types.
- Typed Callable parameter signatures with contravariant inputs, covariant
  returns, zero-argument lists, generic inference, and diagnostic rendering.
- Generic inference from persistent Array and Hash contracts, including empty
  collections and recursively bound contracts from earlier generic calls.
- Explicit generic arguments for function and method calls, including nested
  types, outer generic forwarding, strict arity, and specialized bytecode.
- Reusable modules with class inclusion, receiver-aware generic methods,
  deterministic precedence, inheritance integration, and name diagnostics.
- Transitive module-to-module inclusion with imported/direct precedence and
  self-cycle diagnostics.
- Lexical module namespaces with nested modules/classes, `Outer::Name`
  resolution, qualified type annotations, and isolated local names.
- Symbolic module instance fields resolved through receiver-class field tables,
  shapes, and caches, including transitive state requirements.
- Nested interfaces and write-once namespace constants with lexical/qualified
  lookup, arbitrary expression values, cross-frame access, and GC rooting.
- Module singleton functions with qualified dispatch, defaults, typing,
  generics, nested namespaces, and strict separation from included methods.
- Class singleton methods with generic specialization, defaults, independent
  overrides, and superclass-chain lookup alongside dedicated constructors.
- Private instance methods across classes, inheritance, and modules, restricted
  to explicit `self` calls from method frames.
- Public/private declaration toggling with named private-call diagnostics.
- Generated `attr_reader`/`attr_writer` methods for classes and modules, using
  numeric or symbolic fields while retaining visibility and inheritance.
- User-defined writer methods with typed/default parameters, module state, and
  private self-dispatch, while preserving endless-method parsing.
- Combined `attr_accessor` generation for class and module fields.
- Optional gradual type contracts on generated attribute readers and writers.
- Generated `attr_predicate` readers backed by ordinary instance fields.
- Comma-separated multi-name reader, writer, and accessor declarations.
- Targeted multi-name public/private changes for locally defined and generated
  methods without altering subsequent-definition defaults.
- `module_function` exports that preserve signatures and generics while making
  includable copies private and avoiding a fabricated module instance.
- Scoped standalone `module_function` mode for exporting subsequent methods.
- Duplicate generated attribute-method diagnostics for classes and modules.
- Compile-time rejection of stateful `module_function` exports.
- Class and module singleton writer definitions and qualified invocation.
- Targeted visibility changes for generated and handwritten writer methods.
- Parenthesized reader, writer, and accessor declaration syntax.
- Parenthesized targeted visibility syntax.
- Parenthesized multi-target `module_function` exports.
- Writer-method `module_function` targeting and qualified export calls.
- Reader-only `attr` shorthand with bare and parenthesized multi-name forms.
- Descriptor-level `alias_method` support for class and module instance methods.
- Writer-method sources and targets in `alias_method`.
- Parenthesized `alias_method(new_name, existing_name)` declarations.
- Predicate function and method names ending in `?`.
- Bang function and method names ending in `!`.
- Suffixed-name integration with visibility, aliasing, and module exports.
- `unless` conditional expressions with `else` and type narrowing.
- `until` loops with ordinary `break` and `next` control flow.
- Loop-body `redo` without condition reevaluation.
- Value-bearing `break` results for `while` and `until` expressions.
- Unconditional `loop do` expressions with value-bearing exits.
- Newline-delimited unconditional loops and delimiter diagnostics.
- `next` integration for unconditional loops.
- Nested unconditional-loop target isolation.
- Chained `elsif` branches in conditional expressions.
- Optional `then` delimiters for conditional expressions.
- Optional `do` delimiters for `while` and `until` loops.
- Unary `not` keyword alongside `!`.
- Short-circuit `and` and `or` keyword operators.
- Normal-completion `else` branches in `begin`/`rescue` expressions.
- Bare re-raise of the current rescued exception.
- Rescue-local `retry` with stable ensure-frame behavior.
- Binding-free typed rescue filters.
- Ordered multiple rescue clauses with unmatched re-raise.
- Duplicate rescue-filter type diagnostics.
- Unreachable repeated-type diagnostics across rescue clauses.
- Normal-only `else` integration with multiple rescue clauses.
- Retry integration from later rescue clauses.
- Single-run ensure integration with multiple rescue clauses.
- Bare re-raise integration from later rescue clauses.
- Ordered union-filter dispatch across rescue clauses.
- Nominal subclass matching across ordered rescue clauses.
- Typed payload fields on user-defined exception subclasses.
- Typed causal chaining on user-defined exceptions.
- Runtime enforcement of exception payload contracts.
- Inherited exception payload accessors and nominal rescue matching.
- Exception payload identity preservation through bare re-raise.
- Exception payload stability across ensure cleanup.
- Message payloads on VM-generated standard exceptions.
- Uniform cause slots on built-in and user exception hierarchies.
- Native message/cause construction for exception instances.
- Exception constructor defaults and strict arity diagnostics.
- Stress-GC rooting for VM-created exception payloads.
- Native exception accessor arity enforcement.
- User initializer precedence over native exception construction.
- Explicit cause preservation through raise and rescue.
- Message payload coverage across standard runtime error classes.
- VM message preservation through bare re-raise.
- Diagnostics for subclass filters shadowed by earlier superclass clauses.
- Postfix `if` and `unless` modifiers with direct bytecode layout.
- Postfix modifiers on `break`, `next`, `redo`, `return`, `raise`, and `retry`.
- Nested-loop and rescue-control modifier targeting.
- Endless function and method bodies with postfix `if`/`unless` conditions.
- Explicit diagnostics for postfix modifiers on declarations and namespace
  constants.
- Explicit diagnostics for trailing syntax on standalone `require` directives.
- CRLF-safe `require` scanning and normalized relative-path coverage.
- Nested-import source mapping and missing-dependency context.
- Explicit loader diagnostics for nesting, file-count, segment, and source-size
  limits.
- Circular-import diagnostics include the closing require site.
- Named loader depth/file-count limits and overflow-safe source embedding.
- Require-depth and loaded-file limit regression coverage.
- Root-file seek/read and directory diagnostics preserve OS detail.
- Root file-size failures and imported diagnostics remain consistent in dump
  mode.
- Source-segment range validation and reusable bundle cleanup.
- Canonical source-map path preservation across segment metadata.
- Imported EOF diagnostics retain the dependency path at segment boundaries.
- Imported diagnostic excerpts omit internal loader marker lines.
- Multiline imported EOF diagnostics compute line numbers within the segment.
- Imported runtime failures retain their VM function frames.
- Imported runtime frame coverage includes stress-GC and root callers.
- Imported runtime frames preserve source line and column locations.
- CRLF imported dependencies preserve the same runtime/compile-time mapping.
- CRLF imported runtime frames preserve line and column locations.
- Sequential all-build validation covers debug, release, and sanitizer modes.
- All-build validation cleans between compiler-mode transitions.
- Diagnostics audit complete across compiler, loader, runtime, source maps, and
  all supported build modes.
- Opcode execution profiling and opt-in dynamic integer-add quickening.
- Guarded addition quickening with safe polymorphic deoptimization.
- Guarded integer quickening for dynamic subtraction, multiplication, and division.
- Guarded integer quickening for dynamic ordering comparisons.
- Configurable quickening warm-up thresholds with environment control.
- Guarded integer quickening for equality and inequality.
- Inherited-method and repeated-hit coverage for polymorphic dispatch caches.
- Per-site method-cache telemetry for specialization decisions.
- Monomorphic method-cache fast path with execution telemetry.
- Method-cache probe-depth telemetry for deterministic dispatch comparisons.
- Configurable monomorphic dispatch warm-up thresholds.
- Guarded `INVOKE_MONO` rewriting for stable instance call sites.
- Monomorphic invoke deoptimization coverage when a new receiver class appears.
- Rewrite warm-up suppression coverage for high dispatch thresholds.
- Polymorphic-site guard coverage preventing unsafe invoke rewriting.
- Deterministic 100-call dispatch probe benchmark.
- Explicit rewritten-site tracking for safe VM reuse.
- Reusable-VM repeat execution mode for lifecycle tests.
- Repeated polymorphic rewrite/deoptimization lifecycle coverage.
- Per-run reusable-VM cache and rewrite counter telemetry.
- Immediate-versus-delayed dispatch warm-up baseline coverage.
- Active dispatch-policy trace for reproducible threshold experiments.
- Default one-observation policy locked by deterministic benchmark coverage.
- Explicit method-cache invalidation API for future class/module mutation.
- Repeat-mode exercise of the explicit invalidation boundary.
- C-level method-table mutation and invalidation harness.
- C-level inherited-method mutation and invalidation coverage.
- Selective inherited-method mutation coverage across multiple call sites.
- Method arity and visibility mutation coverage through explicit invalidation.
- Superclass-link mutation and restoration coverage through explicit invalidation.
- Shared-program invalidation coverage across independent VM instances.
- Fiber lifecycle states, frame checkpoints, FIFO scheduler stepping, and
  explicit yield/requeue transitions.
- VM binding, result/status accessors, and one-shot fiber execution boundary.
- Fiber frame register snapshots with bounded checkpoint accessors.
- Explicit fiber execution-context capture and validated restoration.
- VM-facing fresh-context execution boundary with terminal checkpoint updates.
- Explicit VM status propagation through execution contexts and restoration
  guards for nonterminal, successful contexts.
- `run_chunk` context handoff for instruction/register checkpoints on VM exits.
- First explicit `YIELD` opcode with VM status propagation and suspended-fiber
  mapping; continuation-aware re-entry remains next.
- Standalone `yield` source syntax and compiler emission coverage.
- Fiber continuation resumes from saved instruction/register contexts and can
  complete after a terminal yield boundary.
- Sequential multiple-yield fibers with advancing checkpoints and terminal
  completion coverage.
- Scheduler run-once execution with FIFO suspended requeue and terminal
  fiber removal.
- Scheduler run-all draining of an entire fiber queue to terminal states,
  tolerating individual fiber failures without aborting the run.
- Garbage-collection roots for queued and suspended fiber frames, closing the
  use-after-free gap where one fiber's turn could collect another fiber's
  live, checkpointed registers on a shared VM.
- A `diamond_fiber_resumable` predicate and regression coverage confirming
  every lifecycle transition rejects a fiber outside its required source
  state, formalizing the "clear errors for resuming a running or completed
  fiber" requirement stated since the initial fiber design.
- Explicit `DIAMOND_VM_UNSUPPORTED_YIELD` rejection for `yield` below the
  top-level call frame, replacing a silent resume-time correctness bug
  (a nested yield's suspension previously discarded the inner call's
  progress and produced a misleading error on resume) with an immediate,
  deterministic failure. Superseded by the stackful rewrite below.
- Stackful fibers via POSIX `ucontext.h`: each `DiamondFiber` now owns its own
  native OS stack (`mmap`-allocated, with a `PROT_NONE` guard page), and
  `yield`/`resume` are `swapcontext` calls rather than an interpreter-level
  instruction/register checkpoint. `run_chunk` itself is unchanged for nested
  calls, rescue/ensure handling, and generic dispatch, since the OS preserves
  the whole C call stack across a suspend. This fixed two confirmed
  correctness bugs at once: `yield` inside a nested call now resumes and
  completes with the correct value instead of silently corrupting on resume,
  and `yield` inside an active `begin`/`rescue` now preserves the handler
  stack across the suspend instead of losing it. GC root marking was
  generalized to walk any fiber's own parked native frame chain, not just a
  flat register array. This resolves the "Next priorities" item below by a
  different route than originally specified: rather than splitting the
  interpreter into an explicit, hand-persisted frame stack, fibers get their
  own real stack and the interpreter needs no fiber-specific state at all.
- Fixed a real, previously untested crash: `DIAMOND_MAX_CALL_DEPTH` was set to
  256 based only on release-build measurements, but each call activation's
  unconditional multi-kilobyte C locals meant real recursion segfaulted the
  process around depth ~210-220 in a debug build and ~150-160 under
  AddressSanitizer, well before the counter ever tripped. Lowered to 100, a
  margin verified safe under all three build configurations, with new
  regression coverage (previously nonexistent) for both the plain and
  fiber-native-stack execution paths, and confirming `SystemStackError`
  remains rescuable at the new boundary.
- Regression coverage for integer overflow across addition, subtraction,
  multiplication, negation, and the `INT64_MIN / -1` division edge case,
  including the quickened arithmetic fast path and `RangeError`
  rescuability. The checked-arithmetic implementation was already correct;
  none of it had any test coverage beforehand.
- Regression coverage for dynamic-dispatch arity checking: too few/too many
  arguments through `INVOKE`, constructor dispatch, the warmed `INVOKE_MONO`
  fast path, a polymorphic site validating a second receiver class
  correctly, and multi-frame stack traces through nested dynamic calls.
  Direct calls are already compile-time arity-checked and covered; dynamic
  dispatch's runtime check was correct but, like the two gaps above, had no
  test coverage at all.
- A representative mixed-class dispatch workload (`mixed_dispatch_workload.di`,
  a 1000-call loop split 500/500 between two classes) measuring `INVOKE_MONO`
  against polymorphic dispatch at realistic scale: a single deoptimizing
  rewrite when the second class first appears, 498 monomorphic dispatches
  before it, and 1000 polymorphic-cache probes after — locking in that a
  monomorphism break is a one-time cost, not a per-call one. Complements the
  existing small-scale (100-call, 2-call) benchmarks with a larger, mixed
  workload.
- Runtime method redefinition: `ClassName.redefine_method(name, callable)`
  repoints an existing method to a different already-compiled function, with
  cache invalidation happening automatically as part of the call instead of
  a separate manual step. Scoped deliberately narrower than the roadmap
  phrasing might suggest: this repoints methods that already exist, it does
  not define genuinely new ones (`DiamondClass.methods` is a fixed-size,
  compile-time-populated array) and it does not cover modules. Classes are
  not first-class runtime values, so this required a new opcode recognized
  contextually at the same call site that already special-cases
  `ClassName.new(...)`, rather than ordinary method dispatch. Four
  structural checks reject unsafe replacements: the callable must capture no
  variables (a method slot only stores a raw function index, so captured
  state would be silently discarded), it must have been compiled with the
  same `owner_class` as the target (otherwise `self`/`@field` register and
  offset assumptions baked in at compile time would be wrong), its arity
  must exactly match the existing method's, and the name must match an
  existing method on that class directly (no superclass walk, no defining a
  new method). Verified end-to-end from pure Diamond source, including
  against an already-warmed `INVOKE_MONO` dispatch site.
- Phase 1 of Diamond-language fiber syntax: `DIAMOND_OP_YIELD` re-encoded
  with `dest`/`source` register operands (was zero-operand), threading a
  value across the `swapcontext` boundary in both directions at the C level
  (`diamond_fiber_resume(fiber, value)`, `vm->running_fiber->result`). `yield`
  promoted from a statement-only special case to an ordinary primary
  expression (`x = yield(1) + 1` now compiles), fixing a confirmed-broken
  disassembler case (`<unknown opcode 66>`) along the way. This is groundwork
  only — no Diamond-level way to create or resume a fiber exists yet; that is
  the remaining, larger part of this feature (see below).
- Phase 2 of Diamond-language fiber syntax: a fiber is now a first-class,
  GC-managed value, via a new `DIAMOND_OBJECT_FIBER` heap kind wrapping a
  `DiamondFiber` pointer. Both GC-root hazards identified during planning are
  fixed and independently regression-tested (verified against deliberately
  reverted fixes under ASan first): `mark_object` marks a reachable fiber's
  own parked frame chain, result, resume value, and entry closure; and
  `diamond_vm_collect` walks a running fiber's resumer chain, marking each
  ancestor's own frames as GC roots, closing the gap where a value reachable
  only through the resumer's own live registers could be collected out from
  under it while a child fiber's own allocations trigger a collection.
  Sweeping an unreached fiber handle now frees its native stack instead of
  leaking it. See `docs/fibers.md` for the full design. Still no Diamond-level
  way to create or resume a fiber — that is Phases 3-4 (see below).
- Phase 3 of Diamond-language fiber syntax: `Fiber.new(callable)` is now
  Diamond-language syntax, via a new `DIAMOND_OP_FIBER_NEW` opcode recognized
  in the compiler the same way `redefine_method` is (gated on the identifier
  not being shadowed by a local). The callable must be a zero-argument
  `Callable`, checked with a rescuable `TypeError`/`ArgumentError`; captures
  are allowed, unlike `redefine_method`. The dangling-chunk-pointer hazard
  identified during planning is fixed and regression-tested from Diamond
  source three call frames deep (verified against a deliberately reverted
  fix under ASan first: a clean stack-use-after-return). Along the way, an
  unrelated latent bug surfaced and was fixed: `diamond_fiber_prepare`/
  `diamond_fiber_run` unconditionally rejected any fiber with a null
  `chunk`, which closure-invoking fibers always have — nothing had
  exercised that path end-to-end until this phase reached it. Still no
  Diamond-level way to resume, inspect, or free a fiber — that is Phase 4.
- Phase 4 (final) of Diamond-language fiber syntax: `.resume(value)`/
  `.status()`/`.alive?()` native dispatch inside `DIAMOND_OP_INVOKE`, the
  same runtime-kind-keyed mechanism already used for Array/Hash/String
  native methods — no new compiler code needed. A new `FiberError`
  exception class (`StandardError` subclass) backs the one genuinely new
  case with no existing analog: resuming an already-completed or -failed
  fiber, via a new `DIAMOND_VM_FIBER_NOT_RESUMABLE` status. Uncaught
  fiber-body exceptions propagate to the resumer through the existing
  rescue machinery with zero new exception-handling code, since the fiber
  shares the resumer's `DiamondVm`. The user's own generator example round-
  trips end to end from Diamond source (`0, 10, 15`). This closes out
  Diamond-language fiber syntax as a whole (`Fiber.new`/`.resume(value)`/
  `.status()`/`.alive?()`/`yield(value)`, all GC-correct, all tested from
  pure Diamond source). Scheduler (`DiamondFiberQueue`) exposure to Diamond
  source remains explicitly deferred — `Fiber.new(...).resume(...)` alone,
  with no scheduler required, is a complete, independently useful unit.

- Basic I/O, first slice: `print(value)`/`puts(value)` (no newline / with
  newline) via a new `DIAMOND_OP_PRINT` opcode, recognized in the compiler
  the same way `Fiber.new`/`redefine_method` are (shadowable by a local or
  user-defined function of the same name, no reserved keyword).
  Stringification reuses the exact mechanism string interpolation already
  uses (a `stringify_value` helper factored out of `DIAMOND_OP_TO_STRING`
  as part of this work, with no behavior change to existing interpolation):
  a user-defined `to_s` is respected, including its arity being validated
  the same as any other call. See `docs/io.md` for what's covered and what
  remains deliberately out of scope (stdin, files, sockets, multiple
  arguments, write-failure error handling).
- stdin: a `gets()` primitive reading one line from standard input, via a
  new `DIAMOND_OP_GETS` opcode. Returns a `String` with the trailing line
  ending stripped (both `\n` and `\r\n`, correct regardless of internal
  read-buffer boundaries since stripping happens on the accumulated line,
  not per underlying `fgets` call), or `nil` only when zero bytes were
  read before EOF — a final line with no trailing newline still returns
  its content, matching Ruby's `gets`. Same shadowing precedent as
  `print`/`puts`. Files and sockets remain out of scope; see `docs/io.md`.
- File I/O: `File.open(path, mode)` is a first-class, GC-managed value
  (new `DIAMOND_OBJECT_FILE` heap kind wrapping a `FILE *`, same shape as
  `Fiber`'s `DiamondFiberHandle`, via a new `DIAMOND_OP_FILE_OPEN`
  opcode), with `.read()`/`.gets()`/`.write(value)`/`.close()` native
  dispatch and a new `IOError` exception class for open and I/O
  failures. `mode` passes straight through to `fopen`, unvalidated.
  `.gets()` shares the exact `read_line` helper factored out of stdin's
  `gets()` as part of this work (pure refactor, no behavior change).
  Sweeping an unclosed handle closes it as a safety net, matching how
  sweeping an unreached fiber frees its native stack — unlike `Fiber`,
  no `mark_object` branch is needed, since nothing inside a
  `DiamondFileHandle` references another Diamond value. See `docs/io.md`
  for the full surface and what remains out of scope (sockets, mode
  validation).
- Sockets: `TCPSocket.connect(host, port)`/`TCPServer.listen(port)`/
  `.accept()`, blocking TCP only (deliberately no non-blocking I/O, UDP,
  or TLS), via new `DIAMOND_OP_TCP_CONNECT`/`DIAMOND_OP_TCP_LISTEN`
  opcodes and a new `DIAMOND_OBJECT_LISTENER` heap kind for listening
  sockets specifically. The key reuse: a connected socket (from either
  `.connect` or `.accept()`) is `fdopen()`'d and wrapped in the *same*
  `DiamondFileHandle` `File` uses, so `.read()`/`.gets()`/`.write(value)`/
  `.close()` needed zero new code — only connection establishment
  (`connect`/`bind`/`listen`/`accept`) is genuinely new. Both
  constructors resolve via `getaddrinfo` (IPv4/IPv6-agnostic), trying
  every candidate address in turn, and raise a rescuable `IOError`
  preserving the real `strerror(errno)` from the failing attempt. This
  is the prerequisite for the Rack-style web server idea under Later
  experiments. See `docs/io.md` for the full surface.
- Real `Enumerable`: `select`/`count`/`any?`/`all?`/`reduce`/`map`,
  derived from a single `each`, replacing the pain point of today's flat
  `array_*`/`hash_*` free functions with inconsistent naming. Array/Hash
  are native VM object kinds without method tables, so `values.each(cb)`
  receiver syntax resolves through a new runtime mechanism —
  `DIAMOND_OP_INVOKE`'s existing Array/Hash dispatch recognizes a small
  fixed method-name set and forwards to an ordinary top-level prelude
  function by name (`find_top_level_function`, mirroring the compiler's
  own `find_function` filters) — while a new `module Enumerable` gives
  any user class the same six methods via ordinary `include`, both
  routes calling the exact same `enumerable_*` functions with zero
  duplicated logic. A hash receiver's Enumerable operates over values
  only, discarding keys (matching the pre-existing `hash_map_values`
  convention, a deliberate Ruby divergence). Building this feature found
  a real, previously-unexercised compiler bug — a nested `def` inside one
  branch of an `if`/`else` could capture a stale register from a
  *sibling* branch's own nested `def` — worked around at the time by
  declaring both branches' closures unconditionally before branching,
  then the underlying bug itself was fixed (see below), letting this
  code move back to the natural pattern. See `docs/design.md`'s
  "Enumerable" section for the full mechanism.
- Fixed the nested-`def`-in-sibling-branches closure capture bug found
  above. Root cause: capture boxing (`DIAMOND_OP_BOX_LOCAL`) was only
  ever emitted once per local, at the first capture site encountered
  during compilation, on the assumption that a single compile-time
  "already boxed" flag reliably predicts runtime state — which breaks
  when that first site is in a branch that doesn't dominate a later
  capture site in a sibling branch, so on a path that skips the first
  branch, its boxing never ran, and the later closure captured a raw
  unboxed value where a `Cell` was expected. Fixed by making
  `DIAMOND_OP_BOX_LOCAL` idempotent (a no-op if the register already
  holds a `Cell`) and always emitting it at every capture site instead
  of skipping already-flagged locals. Verified against a deliberately
  reverted fix first: the original repro reproduces the exact original
  failure without the fix and passes with it, on both branches.
- Basic Rack-style web server: `lib/http.di`, opt-in via `require
  "lib/http"` (not auto-embedded like `lib/core.di`, so programs that
  never touch HTTP don't spend any of the small 64-entry function-table
  budget on it). `http_serve(port, handler)` loops accept/parse/handle/
  respond/close over a `TCPServer`; request/response convention mirrors
  Rack directly (a request `Hash` of `method`/`path`/`headers`/`body`, a
  handler returning a 3-element response `Array` of `[status, headers,
  body]`). Found and closed a real, previously-unflagged prerequisite
  gap while scoping this: Diamond had no string manipulation beyond
  concatenation/interpolation/`.length()` — no indexing, no substring, no
  numeric parsing — needed for even minimal HTTP parsing. Added
  `String#index_of`/`#slice`/`#to_i` and `File#read(n)` (length-limited
  read, for a request body of known `Content-Length`) as prerequisites.
  A connected socket already being a `File` under the hood (see
  `docs/io.md`) meant no new I/O plumbing was needed for the library
  itself. Deliberately basic: no chunked encoding, no keep-alive, no
  malformed-request handling, no routing layer. See `docs/http.md` for
  the full surface and what remains out of scope.
- Package resolution, phase 1: `require "name"` falls back to
  `diamond_packages/name/name.di` (resolved against the process's CWD)
  when the usual relative-file lookup fails — a bare name only, no new
  syntax, and a relative file always wins when both exist. Scoped down
  from the full "packaging format + package manager" idea in `Later
  experiments` after establishing that manifests/versions/fetching each
  need their own separate mechanism (see `docs/packages.md` for the full
  reasoning and what's still out of scope). `src/loader.c`'s `expand()`
  gained the fallback as a small, additive change; no compiler or grammar
  changes were needed since `require` recognition is entirely
  loader-level text scanning.
- Package resolution, phase 2 (manifests): an optional
  `diamond_packages/name/package.di`, compiled and run standalone (no
  `require` support inside one — a manifest is metadata, not a program)
  to get back a `Hash`. A String `name` key must match the package's own
  directory, or the whole `require` fails with a clear error — the actual
  payoff today, since it catches a package copied/renamed incorrectly
  during a manual install. An optional `version` key is validated for
  type only; nothing consumes it yet (see `docs/packages.md`). Found and
  fixed a real, previously-latent stack-overflow risk while building
  this: `diamond_compile`'s `*program = (DiamondProgram){};` materialized
  a 3MB+ temporary on its own stack frame regardless of where the
  destination pointer lives, which was always true but only became
  reachable now that a second `diamond_compile` call (for the manifest)
  runs from inside `expand()`'s own recursive call chain while the
  top-level program's `DiamondProgram` is still live further up the
  stack in `main.c`'s `run_source`. Fixed by using `memset` instead,
  which needs no stack-resident temporary.
- Maintenance pass over everything built this session (I/O, Enumerable,
  HTTP, fibers, packages). Found and fixed real bugs, not just coverage
  gaps: `diamond_vm_free` leaked a Fiber's native stack / a File's stream
  / a Listener's fd at normal process exit (only the GC sweep path freed
  them) — fixed, and `test-sanitize` now runs with LeakSanitizer enabled
  by default so this class of bug is caught automatically going forward,
  not just by a hand-written case. `read_line` never checked `ferror`,
  so a genuine stream read error on `gets()`/`File#gets` was
  indistinguishable from clean EOF — fixed to match `File#read`'s
  existing check. `lib/http.di` looked up `Content-Length` with exact-
  case `Hash` indexing; a legal lowercase `content-length` header
  silently produced an empty body with no error — fixed by normalizing
  header names to lowercase at parse time (added `String#downcase` for
  this). Also gave `File`/`Listener`/`Fiber` the same descriptive
  "undefined method 'x' for Y" error on an unrecognized method that
  Array/Hash/String already had, let a top-level function shadow
  `Fiber.new`/`File.open`/`TCPSocket.connect`/`TCPServer.listen` the same
  way one already could shadow `print`/`puts`/`gets`, and closed out the
  coverage gaps a full survey found (manifest failure paths, `DIAMOND_
  STRESS_GC=1` for packages and Enumerable, empty-collection Enumerable
  cases, I/O type-error messages, malformed builtin syntax).
- Stdlib round 1: collections + numeric helpers. Thirteen new
  `lib/core.di` functions, all pure Diamond built from existing
  primitives — zero VM/compiler changes. Array: `reverse`/`concat`/
  `compact`/`uniq`/`flatten` (fully recursive)/`join(separator = "")`/
  `delete_at` (mutates in place, `nil` on out-of-bounds rather than
  raising, matching Ruby). Hash: `merge` (right-biased on conflicts).
  Numeric: `abs`/`min`/`max`/`mod` as plain functions, not receiver
  syntax — `Int` is a scalar `DiamondValue`, not a heap object, so
  there's no per-value method dispatch for it. `array_sort` is Int-only
  (checked via the existing `Array[Int]` parameter-type mechanism,
  `array_map_int`'s precedent) since `Int` is the only type with a
  native ordering comparison — confirmed during scoping that Diamond
  has no `Float`, no operators beyond `+ - * /`, no `Range`, and no
  sorting or Comparable/spaceship mechanism at all, each requiring new
  lexer tokens/opcodes/a `DiamondValue` layout change rather than a
  stdlib addition, so out of scope for this round. `lib/core.di` is now
  at roughly 50 of the 64 `DIAMOND_MAX_FUNCTIONS` slots — a real
  constraint on how much further pure-Diamond stdlib growth this budget
  can absorb before it needs raising.
- Stdlib round 2: String primitives. `.upcase()`/`.reverse()`/`.strip()`/
  `.split(separator)`/`.ord()` as new `DIAMOND_OP_INVOKE` branches in
  `src/vm.c`'s String dispatch block, following the exact `.downcase()`/
  `.slice()` pattern (`.split`'s empty-separator case splits into
  one-character strings; otherwise keeps every piece including empty
  ones, deliberately not replicating Ruby's trailing-empty-suppression
  quirk). `chr(code)` is the inverse of `.ord()` — a new global function
  (not receiver syntax, since `Int` has no per-value method dispatch)
  backed by a genuinely new `DIAMOND_OP_CHR` opcode and compiler
  recognition mirroring `gets()`/`print()`, since there was no existing
  Diamond-level way to build a `String` from a raw byte value. Scoped
  down from the full round-2 target list: char-indexing (`"abc"[0]`)
  and string repeat (`"x" * 3`) were deliberately left out because both
  would touch shared, high-traffic opcodes (`INDEX_GET`/`INDEX_SET`,
  `MULTIPLY`'s quickening-instrumented block) with real regression-risk
  surface for Array/Hash/Int, unlike every other addition this round
  which was a purely additive, isolated branch — see `Next priorities`.

- Stdlib round 3: char-indexing and string repeat, the two String
  primitives deferred from round 2. Research before implementing found
  the two weren't equally risky: `DIAMOND_OP_INDEX_GET`/`INDEX_SET`
  turned out to have no quickening or specialization at all — a plain
  receiver-kind dispatch — so `"abc"[0]` (bounds-checked, returns a new
  one-character `String`) and an explicit `TypeError` rejection of
  `"abc"[0]=...` (strings are immutable) were purely additive branches,
  no different in risk from round 2's methods. `DIAMOND_OP_MULTIPLY`
  was genuinely riskier: the compiler can statically emit
  `MULTIPLY_INT` whenever both operands' types are known `Int`
  (independent of the `DIAMOND_QUICKEN` env var), and unlike `ADD_INT`,
  `MULTIPLY_INT` has no deopt-back-to-generic path at all — a call site
  already specialized that later saw a String would hard-crash with no
  string-repeat handling, and building a safe deopt mechanism for it
  would have been real new complexity, not a small addition. Shipped as
  `.repeat(n)` instead (a `DIAMOND_OP_INVOKE` branch, the same shape as
  every other round-2/3 String method) rather than overloading `*` —
  `"x" * 3` is intentionally not supported. See `docs/syntax.md`.
- Replaced `Hash`'s O(n) linear-scan lookup with a real hash table.
  Found via the JIT-branch baseline benchmarks (`hash_find` was a plain
  loop over every entry — no hashing happened anywhere despite the
  name); fixed on `main` since it's a data-structure issue, not a
  bytecode-dispatch one. Kept the existing insertion-ordered entry
  array exactly as-is (so `key_at`/`value_at`, GC marking, and every
  `lib/core.di` Hash function needed zero changes) and added a
  separate open-addressing bucket table alongside it mapping each key's
  hash to an entry index — the same shape CPython's `dict` uses
  internally to stay insertion-ordered while still being O(1) average.
  See `docs/design.md` for the exact mechanism. Confirmed at scale:
  1,000,000 Hash inserts + lookups now complete in well under half a
  second, versus effectively unbounded time under the old O(n²)
  behavior at that size.
- Added `Float`, a single IEEE-754 double-precision type (Ruby/Python/JS
  style — not a separate single/double pair like Java's `float`/`double`).
  Adds no memory cost (`DiamondValue`'s union already had an 8-byte slot
  free) and no new arithmetic/comparison opcodes: the existing `_INT`
  compiler specialization only fires when both operands are provably
  `Int`, so `Float` code — static or dynamic — already fell through to the
  generic opcode paths, which were extended to handle `Float`/mixed
  operands directly rather than building a parallel quickened `_FLOAT`
  tier the project's own baseline benchmarks say would not pay for
  itself (see `bench/BASELINE.md` on the JIT-experiment branch). Mixed
  `Int`/`Float` arithmetic and comparisons auto-promote the `Int` side.
  `to_f`/`to_i` convert explicitly; `to_i` rejects `NaN`/`Infinity`/
  out-of-range values with a rescuable `RangeError` rather than
  triggering undefined behavior in the C cast. Deferred to a later
  round: exponent-notation literals (`1e10`) and shortest-round-trip
  formatting (this round uses a fixed `%.15g`-based format).
- Interpreter call-overhead reduction, from `jit-experimentation`
  baseline benchmarking that found call/frame-setup overhead — not
  opcode-level arithmetic dispatch — was the dominant cost in call-heavy
  code (existing `_INT` quickening measured no benefit even in code
  built specifically to exercise it). Landed three compounding wins in
  `run_chunk`, the single interpreter loop: narrowed its per-call
  register zero-init from the fixed 256-slot array down to each
  function's actual `register_count` high-water mark (+10-31% on
  call-heavy benchmarks); elided 11 `DIAMOND_OP_NIL` emissions now
  provably redundant with that narrower zero-init (+5-8%); and removed
  a second, wholly separate `DiamondChunk` struct copy `run_chunk` built
  on every call but only read for generic functions (+10-13.5%, a
  bigger win than the first two rounds' magnitude predicted — evidence
  that eliminating dead work can unlock further compiler optimization
  of the surrounding function, not just save its own literal cost). A
  fourth, more invasive attempt — eliminating the *external* per-call
  `DiamondChunk` construction at each call site by splitting
  `run_chunk`'s single chunk parameter into separate function/program
  pointers — was fully implemented and verified correct, but measured
  as a confirmed ~9% regression on method dispatch (more `run_chunk`
  parameters likely means more register pressure across its entire
  body, not just at call sites) and was reverted rather than landed.
  Full methodology, numbers, and the reverted attempt's postmortem in
  `bench/BASELINE.md`.
- `Float` siblings for `abs`/`min`/`max`/`mod` (now `Int | Float`,
  auto-promoting) and `String#to_f`, closing out the Float milestone's
  own deferred list. `abs`/`min`/`max` needed no body changes — their
  logic was already just generic comparisons and negation, both
  already kind-dispatching for `Float`. `mod`'s old formula
  (`a - (a / b) * b`) relied on `/` truncating, true for `Int` but not
  `Float` (exact division makes it algebraically collapse to always
  `0`); fixed by explicitly truncating the quotient toward zero via
  `to_i`/`to_f` before multiplying back, preserving the same C-style
  sign convention `Int`'s `mod` already had rather than introducing a
  new one. `String#to_f` mirrors `String#to_i`'s existing dispatch
  (native, in `src/vm.c`'s `INVOKE` string-method chain) but leans on
  `strtod` for the actual parsing rather than hand-rolling it, gated
  on a leading-character check so it doesn't inherit `strtod`'s
  whitespace-skipping (kept consistent with `to_i`'s no-skip
  convention). Found and worked around a genuine, pre-existing parser
  limitation along the way: `postfix_modifier_ahead` (`src/compiler.c`)
  detects a trailing `stmt if cond` postfix modifier via a lexer-only
  lookahead that scans the rest of the current line for a top-level
  `if`/`unless` token, with no assignment-structure awareness — so
  `x = if ... end` (an `if`-expression as an assignment's right-hand
  side, on the same line as the `=`) misparses, treating the RHS's own
  `if` as a misplaced trailing postfix condition. `if` as a
  statement's own leading token (the existing, working pattern
  throughout `lib/core.di`) is unaffected. Worked around by keeping
  `if`/`else`/`end` as the whole statement rather than an assignment's
  RHS; the parser bug itself is unfixed and would need
  `postfix_modifier_ahead` to become structure-aware rather than a
  flat token scan.
- A small Math library — `sqrt`/`sin`/`cos`/`tan`/`pow` — closing out
  the Float milestone's last deferred item. Two new opcodes rather
  than five: `DIAMOND_OP_MATH_UNARY`/`DIAMOND_OP_MATH_BINARY` take a
  function-ID byte operand (a `DiamondMathFunction` enum), so future
  additions (`log`, `exp`, `atan2`, ...) reuse the same two opcodes
  instead of needing a new one each — the `chr`/`to_f`/`to_i`
  one-opcode-per-primitive precedent doesn't scale as well once
  there's a genuine *family* of similarly-shaped functions. All
  arguments accept `Int | Float` (auto-widening); results are always
  `Float`, even for all-`Int` input (`pow(2, 10)` is `1024.0`), rather
  than adding "sometimes-`Int`" result logic. No extra validation —
  results follow IEEE-754 directly (`sqrt(-1.0)` is `NaN`), matching
  `Float` arithmetic's existing philosophy. Needed one build-system
  fix: `sqrt`/`sin`/`cos`/`tan`/`pow` are real exported libm symbols
  requiring `-lm` at link time (unlike `isnan`/`isinf`, which are
  compiler builtins and need no library at all) — added a shared
  `LDLIBS` Makefile variable applied to every build and test target.
- Exponent-notation literals (`1e10`, `1.5e-3`, `2E+7`) and
  shortest-round-trip `Float` formatting, closing out the Float
  milestone's deferred list entirely. The lexer change is purely
  additive: after the existing digit-run/`.`-fraction scan, peek for
  `e`/`E` optionally followed by `+`/`-` and at least one digit,
  same "peek before committing" shape the `.`-fraction check already
  uses (a bare `5e` still leaves `e` for the next token rather than
  erroring). `parse_float` needed no changes at all — it already
  copies the token span into a buffer and calls `strtod`, which
  handles exponent notation natively once the token span covers it.
  Formatting replaced the fixed `%.15g` (not actually always
  sufficient to round-trip an arbitrary `double` — 17 significant
  digits is the number that provably is) with an iterative search:
  try `%.*g` at precision 1 through 17, re-parse with `strtod`, stop
  at the first precision whose bit pattern (not `==`, so `-0.0`/`0.0`
  stay distinct) matches exactly. Needed one correction after the
  initial version: `%g` switches to scientific notation once its
  precision is less than or equal to the value's own decimal exponent,
  so starting the search at precision 1 unconditionally made round
  values like `10.0` hit that threshold immediately (`"%.1g"` →
  `"1e+01"`, which round-trips exactly and so ended the search on the
  wrong notation). Fixed by probing the exponent first via `"%.0e"`
  (not `log10` — `log10(10.0)` can land a hair under `1.0` and round
  the wrong way) and starting the search at a precision that keeps
  `%g` in fixed-point mode for that magnitude — but only when doing so
  can actually change the outcome (exponent 1–16); for larger
  exponents `%g` would use scientific notation at every precision up
  to 17 regardless, so starting at 1 there instead finds the
  genuinely shortest scientific form (e.g. a 70-digit `Float` literal
  one below `1e70` is exactly the same `double` as `1e+70`, and should
  print that way, not as a needless 17-digit mantissa). Two
  already-landed tests had been silently relying on the old fixed
  15-digit format's rounding rather than an exact round-trip
  (`pow(2.0, 0.5)`, whose old expected output `"1.4142135623731"` does
  not parse back to the same bits as `sqrt(2)`) and were corrected to
  their exact values as part of this change.
- Structural interfaces and more capable flow typing (three
  independent gaps, picked out of several found while investigating
  this open-ended "Later experiments" line item):
  - `if x is Foo && y is Bar` now narrows both `x` and `y` inside the
    branch where the whole condition is true, and the `unless`/`||`
    mirror narrows both where the whole condition is false. Previously
    only a single pending narrowing fact was tracked at all
    (`Compiler.narrowing` in `src/compiler.c`), and compiling the
    right-hand side of `&&`/`||` silently overwrote it before `parse_if`
    ever saw it — so a composed condition narrowed neither operand.
    Generalized to two small per-condition fact lists (`when_true`/
    `when_false`, capped at 8 entries, silently stops merging past that
    rather than erroring); `&&` composes `when_true` (both sides must
    hold — sound), `||` composes `when_false` (both must have failed —
    sound); the unsound direction of each (the disjunctive failure case
    of `&&`, the disjunctive success case of `||`) is left empty rather
    than guessed, which is what makes this compose correctly through
    chains and mixed `&&`/`||` automatically with no special-casing —
    an empty list contributes nothing when concatenated into an outer
    expression's facts.
  - `interface Sub < Base1, Base2` composes interfaces by flattening
    each base's method signatures into the new interface at compile
    time (`compile_interface`, `src/compiler.c`) — no runtime interface
    hierarchy, so zero changes were needed to either matching function
    (`known_type_satisfies_one` in `compiler.c`, `value_matches_type`/
    `runtime_type_id_satisfies` in `vm.c`); a composed interface looks
    identical to one written out by hand once compiled. Duplicate
    method names (from two bases, or re-declaring an inherited one) are
    rejected with the same "duplicate interface method" error a single
    interface's own body already used for local duplicates.
  - Structural matching against native `String`/`Array`/`Hash` values
    was a 5-method hardcoded whitelist (`length`/`push`/`pop`/`key_at`/
    `value_at`), duplicated across three call sites, so any other real
    native method — `.slice`, `.strip`, `.to_i`, `.split`, etc. — could
    never satisfy an interface even though the VM implements it.
    Replaced with one canonical table (`DIAMOND_NATIVE_METHODS` in
    `src/vm.c`) covering the VM's actual native method surface, queried
    through a single new function (`diamond_native_method_satisfies`,
    declared in `vm.h`) from all three call sites. A native method whose
    return type isn't one fixed scalar (`pop`, `key_at`, `value_at`,
    `String#index_of`) can still satisfy an interface method with no
    return annotation, matching the existing conservative-return
    convention already used for user-class methods — just never one
    that requires a specific return type.
- `facet`, a standalone package-manager CLI (`tools/facet.c`, its own
  binary at `build/facet`, deliberately not part of the `diamond`
  runtime or its release — the same relationship RubyGems' `gem` has to
  `ruby`), closing out the package-manager line item's fetch/install/
  lockfile pieces. Written in C rather than Diamond itself since Diamond
  has no HTTP client or process-spawning primitive yet (and doesn't need
  either now — see below); links the compiler+VM as a library exactly
  like the existing `tests/*.c` binaries already do, reusing
  `diamond_compile`/`diamond_vm_run` to read `package.di`/`facet.lock`
  the same way `src/loader.c`'s `validate_package_manifest` already
  evaluates manifests, rather than writing a second parser. Packages are
  identified by git URL, not a registry — no index service to stand up;
  `git clone`/`checkout` run via `fork`/`execvp` (argv array, never a
  shell, so a URL/ref pulled from a manifest can't be interpreted as
  shell syntax). Full design and rationale in `docs/packages.md`,
  including why resolving a dependency graph here is inseparable from
  fetching it (no registry to query metadata from without a clone) and
  why a ref conflict between two requesters is a hard error rather than
  something resolved (the same flat-namespace constraint that keeps
  `version` unconsumed — two versions of one package could never coexist
  in a single compiled program, so there's no "resolve to whichever"
  fallback). Found and worked around a genuine, pre-existing parser
  limitation along the way: Diamond's `Hash`-literal parser doesn't
  accept a newline between `{` and its first entry, so `facet.lock` (and
  any manifest) must be single-line — documented in `docs/packages.md`
  rather than fixed, out of scope for this round.
- Extracted `lib/http.di` out of the main repo entirely into
  `diamond-http`, a standalone package in its own sibling git repo,
  installable via `facet` (see above) rather than bundled with the
  runtime — the position that Diamond itself shouldn't ship an HTTP
  server at all, now that `facet` exists to make "install it as a
  dependency" a real option instead of a hypothetical one. No functional
  change to the library itself: same functions, same behavior, just a
  new home (`require "http"`, not `require "lib/http"`) and its own
  `package.di`/README/test script. The four HTTP tests that lived in
  this repo's `tests/run.sh` moved with it, adapted into the new repo's
  own `test.sh`. `docs/http.md` is gone from this repo along with it —
  the library's documentation now lives with the library, in the new
  repo's own README.
- Fixed the `Hash`-literal newline limitation flagged in the previous
  round: bracket-delimited, comma-separated lists across
  `src/compiler.c` failed to parse when a newline appeared right after
  the opening bracket, right after a comma, or right before the
  closing bracket — not just `Hash` literals as originally scoped, but
  the identical defect in array literals and every call-argument list
  too, confirmed by direct testing (`[1,\n2]`, `add(1,\n2)` both failed
  identically). Root cause: these parsing loops never called the
  existing `skip_newlines` helper (already used elsewhere for
  `then`/`do`/block-start newlines) at the three points a
  bracket-delimited list needs it — right after the opening bracket,
  right after a comma, and right before the closing bracket (the third
  point turns out to be subsumed by placing the second one immediately
  after parsing each element, before checking for a comma, rather than
  needing a separate check). Applied uniformly across every genuinely
  bracket-delimited construct: array/hash literals, every call-argument
  list (closures, named functions, methods, `super`, `ClassName.new`,
  module singleton calls), generic type-argument/parameter lists,
  `def`/interface parameter declarations, `Array[T]`/`Hash[K,V]`/
  `Callable[...]` type annotations, and the small fixed-arity builtin
  calls (`File.open`, `TCPSocket.connect`, `redefine_method`, `chr`,
  `to_f`, `to_i`, `sqrt`/`pow`/etc., `Fiber.new`, `TCPServer.listen`).
  One class of site needed a narrower, conditional fix rather than the
  same three-point pattern: `attr_accessor`/`private`/`module_function`/
  `alias_method` accept an *optional* `(...)` — for the unparenthesized,
  bare form (`attr_accessor a, b`), a bare trailing newline is what
  already correctly ends the statement, so newline-skipping there is
  gated on `parenthesized` being true, applied only within that branch,
  leaving the bare form's behavior completely untouched. `interface
  Sub < Base1, Base2`'s base-interface list was confirmed but
  deliberately left alone — it isn't bracket-delimited at all (no
  closing token to skip toward), a different fix shape out of scope for
  this round. Verified the change doesn't touch newline handling
  *inside* a nested block expression appearing as a list element (e.g.
  a multi-statement `if...end` as a hash value): the fix only ever
  skips newlines at a list's own boundary-checking points, never
  inside `parse_expression`'s own recursive handling of an element's
  value, which already had its own correct newline handling untouched
  by any of this.
- Follow-up: `interface Sub < Base1, Base2`'s base-interface list, the
  one site explicitly deferred from the round above for having no
  closing bracket to skip toward. The actual gap turned out narrower
  than that made it sound: a trailing newline after the last base name
  (`interface B < A\n def bar()\nend`) already worked, because falling
  out of the loop (a newline is never `,`, so the loop's own
  "not-a-comma" exit condition already handles it) lands directly on
  `consume_block_start`, which already expects and skips exactly that
  newline before a block body. The only real failure was a newline
  right after `<` or right after a comma (`interface B <\n A,\n X`),
  fixed with two `skip_newlines` calls — after consuming `<`, and after
  consuming a comma. Deliberately did **not** add a third call right
  after consuming a base name, before the comma check (the shape every
  other site in the prior round uses): that would consume the same
  trailing newline `consume_block_start` needs to see still present,
  regressing the case that already worked, since `consume_block_start`
  fails outright if `current` isn't already a literal newline token
  when it runs (it doesn't tolerate having been skipped past already).
- Fixed the long-documented `postfix_modifier_ahead` limitation:
  `x = if cond ... end` (an `if`/`unless`-expression as an assignment's
  RHS, on the same line as `=`) now parses correctly, instead of
  misreading the RHS's own `if` as a misplaced trailing postfix
  condition. Two independent gaps in the same lookahead scan, both in
  `src/compiler.c`: (1) the scan's bracket-depth counter starts at 0
  even when `compiler->current` itself is already an unclosed opening
  bracket (e.g. a bracketed literal that's the *entire* statement, like
  `[if x ... end]`), so a nested `if` at the literal's own top level
  was miscounted as depth 0 and misread as a modifier on the whole
  statement — fixed by seeding `depth` to 1 when `current` is `(`/`[`/
  `{`. (2) An `if`/`unless` found immediately after `=` at depth 0 is
  the start of the assignment's own RHS, not a modifier on a
  not-yet-parsed value — fixed by tracking whether the previous
  depth-0 token was `=`, and treating a same-position `if`/`unless` as
  "no modifier here" rather than triggering on it. Deliberately did
  **not** extend the same "expression expected" tracking to `return`/
  `raise`: unlike assignment (which has no "bare, valueless" form),
  `return if cond`/`raise if cond` already have an established,
  different meaning — a bare return/raise, postfix-conditioned on
  `cond` — that `compile_return`/`compile_raise` already implement via
  their own `current.kind==IF`/`UNLESS` branches; an initial attempt at
  extending the fix to `return`/`raise` broke that existing behavior
  before being caught by the existing test suite and reverted. Getting
  an `if`/`unless`-expression's *value* out of a `return`/`raise`
  still needs explicit parens (`return (if cond ... end)`), matching
  the deliberate ambiguity resolution already in place. Also
  deliberately narrower than fully general: an `if`/`unless`
  immediately after any operator *other* than `=` (e.g.
  `x = y && if cond ... end`) is still misread the old way — the
  `=` case is what the original bug report and every practical
  instance of this actually was.
- Arbitrary-precision integers: `Int` arithmetic overflow now
  auto-promotes to a heap-allocated `DiamondBignum`
  (`DIAMOND_OBJECT_BIGNUM`, sign + base-10⁹ limbs, `src/bignum.c`/
  `src/bignum.h`) instead of raising `RangeError`, the Ruby/Python/Lisp
  style rather than a separate `BigInteger`-like type — `Int` now has no
  user-visible size limit, only a representation that changes
  transparently. `Int` literals in source stay capped at 64-bit (a
  deliberate scope cut, confirmed up front); only runtime arithmetic
  overflow (`+`/`-`/`*`/unary `-`), `String#to_i`, and `to_i` on an
  out-of-range `Float` promote. Every producing operation canonicalizes
  its result back to a plain inline `Int` whenever it still fits
  `int64_t`, so a bignum object only ever exists when genuinely needed.
  Three real bugs surfaced during implementation, not just new code:
  - **Anticipated by the plan**: `SUBTRACT_INT`/`MULTIPLY_INT`/
    `DIVIDE_INT` and the four comparison `_INT` opcodes had no
    deopt-to-generic branch at all before bignums existed (only
    `ADD_INT`/`EQUAL_INT`/`NOT_EQUAL_INT` did) — fine when the only way
    an already-quickened `Int` operand could stop being
    `DIAMOND_VALUE_INT` was a genuine type violation, but not once a
    value can legitimately become a bignum mid-execution. Fixed by
    mirroring `ADD_INT`'s existing deopt branch onto all seven.
  - **Not anticipated**: `ADD_INT`'s *existing* deopt branch doesn't
    retarget-and-fall-through like the new branches above — it handles
    the string-concatenation special case inline and then
    unconditionally returns `TypeError`, so a bignum operand reaching a
    directly-compiled (never-quickened) `ADD_INT` instruction hit that
    early return and never reached `ADD`'s own bignum-aware logic.
    Found via a concrete failing test, fixed by adding an explicit
    bignum check inside that deopt branch before its final return.
  - **Not anticipated, GC-unsafety**: widening two operands to bignums
    back-to-back (`diamond_bignum_from_int64` called once per operand)
    is unsafe — the first widened bignum is reachable from no GC root
    (not yet written into any register) while the second widening's own
    allocation can trigger a collection, sweeping the first one away out
    from under the subsequent arithmetic call. Caught by a real
    AddressSanitizer heap-use-after-free, root-caused against
    `mark_frame_chain`'s exact rooting semantics (only
    `frame->registers[0..count)` are roots). Fixed structurally rather
    than patched per call site: every bignum arithmetic/comparison
    function now takes a `DiamondIntView` (stack-local, non-allocating
    — a small `int64_t` backed by its own array, or an existing
    bignum's limbs referenced directly), guaranteeing at most one
    allocation per call — the final result, if any — never two in a
    row, eliminating the bug class rather than one instance of it. A
    second, subtler bug turned up building that fix: `DiamondIntView`
    is self-referential (`.limbs` points at its own `.small_limbs`
    member), and returning such a struct *by value* doesn't relocate
    that pointer into the caller's copy — the copy's `.limbs` keeps
    pointing at the original, now-dead callee stack slot. Two views
    built this way at the same call site landed on the same reused
    stack address, so both ended up reading the same (garbage) memory —
    caught by manifestly wrong arithmetic results (`INT64_MAX + 1`
    computing to `2`), not a crash. Fixed by making
    `diamond_int_view`/`diamond_int_view_int64` take an out-parameter
    instead of returning by value, so the pointer is always built
    directly inside the caller's real storage.
- Symbols: a `:name` literal (`DIAMOND_OBJECT_SYMBOL`, `src/object.h`) —
  content-compared and content-hashed like `String` rather than Ruby-style
  interned/pointer-equal singletons, a deliberate scope decision (confirmed
  up front) to avoid a new VM-level intern table that would need to be a
  permanent GC root — Symbol gets ordinary GC lifetime instead, matching
  how `String` literals already re-allocate fresh on every execution rather
  than being cached. `to_sym(string)` converts `String` → `Symbol`; the
  reverse direction is free once the shared value formatter
  (`builder_format_value`/`diamond_value_fprint`) gained a Symbol case,
  printing the bare name with no leading colon specifically so
  `to_sym(to_s_output) == symbol` holds as a true round trip (printing
  *with* the colon was the first instinct, and was rejected once it broke
  exactly that round trip).

  The one real lexer subtlety: `:` immediately followed by an
  identifier-start character only begins a Symbol when the colon *isn't*
  glued (no space) onto the end of a preceding identifier, digit, or
  closing `)`/`]`/`}`/`"`. Without that check, a parameter type annotation
  written with no space (`x:Int`), a hash-literal separator with no space
  (`{"a":b}`), and a `rescue e:Type` binding with no space would all
  silently misparse as Symbol tokens — every current use of all three in
  `tests/run.sh` happens to include a space, so this wasn't caught by
  running the existing suite; it was caught by deliberately checking for
  it during planning, mirroring this session's `postfix_modifier_ahead`
  fix's "narrower than fully general, covers the real case" scoping.

  A second, unrelated latent bug was found and fixed along the way, in
  code this same session's bignum round touched: `format_value_type`
  (`src/vm.c`, backs the "expected X, got Y" `CHECK_TYPE` error message)
  had no case for `DIAMOND_OBJECT_BIGNUM` — a bignum value failing a type
  check fell into the function's catch-all branch, which reads
  `((DiamondInstance*)value)->class->name`, reinterpreting a
  `DiamondBignum`'s `bool negative`/`limb_count` fields as a class
  pointer. It didn't crash outright (produced `"got (null)"` rather than a
  segfault, apparently by chance of struct layout/padding) but was
  genuine undefined behavior. Fixed by giving `DIAMOND_OBJECT_BIGNUM` its
  own explicit `"Int"` case, alongside the new `DIAMOND_OBJECT_SYMBOL`
  case this round needed anyway.
- Operator overloading: a class can define `+`/`-`/`*`/`/`/`==`/`<`/`<=`/
  `>`/`>=` as ordinary instance methods, dispatched to from Diamond's own
  operator syntax. Landed with almost no new machinery: `lookup_method`
  already dispatches purely by name-string with no charset restriction, so
  a method literally named `"+"` was already legal at the VM level before
  this round — the only real barrier was `compile_definition` requiring an
  `IDENTIFIER` token right after `def`, broadened to also accept the nine
  binary operator tokens (restricted to inside a class body; an operator
  method has no meaning without a receiver). Unary minus is a method named
  `negate` rather than Ruby's `-@` spelling — a deliberate choice
  (confirmed with the user) to avoid new lexer syntax entirely, since `-`
  alone already names the binary form and arity (0 args vs. 1) already
  tells them apart without needing a name that does too. Dispatch itself
  is one new helper (`invoke_operator_method`, `src/vm.c`) reusing
  `lookup_method_cached` (the same monomorphic-class inline cache
  `INVOKE`/`INVOKE_MONO` already get, for free) and the same
  `DiamondChunk child`/`run_chunk` sub-dispatch shape every other
  same-chunk method call in this file already uses (`to_s`'s dispatch in
  `stringify_value`, the real `INVOKE` case, the `Enumerable`-forwarding
  block) — called from each arithmetic/comparison opcode's existing
  `TypeError` fallback.

  A real bug surfaced immediately on first manual test, not from the test
  suite: `DiamondMethod.arity`/`required_arity` are stored **receiver-
  exclusive** (`compile_definition` stores `function->arity-1` when
  registering a class method — `function->arity` itself includes the
  implicit `self` slot, set before parameter parsing begins). The first
  version of `invoke_operator_method` compared a receiver-*inclusive*
  argument count against those fields directly, so every single overload
  call raised a spurious `ArityError` (`negate()`, arity 0 excluding self,
  compared against 1; `==(other)`, arity 1 excluding self, compared
  against 2) — caught by trying the feature by hand before writing any
  permanent test, not by an existing regression. Fixed by computing the
  explicit (non-receiver) argument count separately for the arity check,
  matching exactly how the real `INVOKE` opcode's own `argc` (also
  receiver-exclusive) is checked against the same fields.

  A second gap, found the same way: `interface`'s own method-signature
  parser (`compile_interface`) has an *independent* `IDENTIFIER`-only
  check for a required method's name, separate from
  `compile_definition`'s — writing `interface Addable\n def +(other)\nend`
  to test that a class satisfies an interface via an operator method
  failed to even parse. Broadened the same way, so an interface can
  require an operator-named method and a class satisfies it through the
  ordinary name-matching interface-satisfaction path, no interface-side
  dispatch changes needed beyond accepting the name.

  Known, deliberately unfixed wart: `ADD_INT`'s deopt branch handles a
  deopted instruction inline instead of falling through to a fresh
  dispatch of the now-generic `ADD` (the same reason the bignum round
  needed its own inline bignum check there) — so the `"+"`
  operator-overload check has to be duplicated a second time inside that
  block rather than living in one place. Not restructured this round
  (would mean merging `ADD`'s case block into the shared
  `SUBTRACT`/`MULTIPLY`/`DIVIDE`/`_INT` one, a bigger and riskier change to
  stable, heavily-exercised arithmetic dispatch than either the bignum or
  this round actually needed).
- Keyword arguments for direct calls to a top-level `def`: `f(x: 1, y: 2)`,
  or mixed with positional arguments (positional first, then keyword).
  Entirely compiler-side, no new opcodes or VM changes — `find_function`
  (the resolver `parse_name`'s call-compiling path already uses) already
  excludes class/module methods and nested closures, matching its runtime
  counterpart `find_top_level_function` exactly, so this call path only
  ever reaches a genuine top-level `def` whose exact signature the
  compiler already has non-polymorphic, compile-time access to — unlike
  `INVOKE` (resolved by the receiver's *runtime* class) or `CALL_CLOSURE`
  (the target value isn't known until runtime), which is why this round
  scopes keyword arguments to direct calls only. `DiamondFunction` gained
  a `parameter_names[16][...]` field (previously only parameter *types*
  were tracked outside a function's own body); the call site resolves
  each keyword to its declared slot and reorders into the same
  positionally-ordered `MOVE`-then-`CALL` sequence a plain positional call
  already emits, so the runtime experiences every keyword call exactly
  like an ordinary positional one. Keyword arguments can't skip an
  earlier defaulted parameter to reach a later one (`f(1, c: 5)` when `b`
  has a default is a compile error) — default values are compiled inline
  into the callee's own bytecode, conditioned on a contiguous supplied
  count from the start, not stored as independently re-evaluable
  expressions a call site could reach around a gap.
- `Regexp`: `Regexp.new(pattern, options = 0)`, `.match(string)` (returns
  `Array[String | Nil]`, `nil` on no match), `.match?(string)`. Backed by
  `reginold`, a companion regex engine (Thompson NFA / Laurikari tagged
  NFA / Onigmo fallback across three tiers, transparent to callers) built
  and maintained as a separate project, linked in via a sibling checkout
  at `../reginold` — the first dependency this project has taken on code
  outside its own repo, an accepted, intentional hard dependency rather
  than an optional one, since `reginold` exists specifically to serve
  this integration. `Regexp.new`/`.match`/`.match?` mirror `File.open`'s
  existing pseudo-class pattern (a magic name recognized at a `.` call
  site, dispatching to a dedicated opcode; the compiled handle wrapped in
  a plain GC-tracked object with no OS resource to explicitly `.close()`,
  simpler than `File`'s lifecycle). No `/pattern/` literal syntax, no
  `String` integration, no richer `MatchData` object — all confirmed,
  deliberate v1 scope cuts, not gaps.

  A real regression surfaced by `make test-sanitize`, not by hand-testing
  (every match/no-match/capture-group/invalid-pattern scenario worked
  correctly on the first try): the initial implementation declared its
  `reginold_error`/`reginold_match` locals directly inside `run_chunk`'s
  own giant opcode switch, the same way every other native-handle opcode
  in this file already does. At `-O0` (this project's debug and sanitize
  builds), every local variable declared anywhere in a switch statement
  contributes to the enclosing function's one stack frame regardless of
  which case actually runs — sibling blocks don't get to share stack
  space the way they might under optimization. `reginold_error` alone is
  ~112 bytes (its message buffer is `REGINOLD_ERROR_MSG_MAX=90`); adding
  it and `reginold_match`'s fields pushed `run_chunk`'s per-level frame
  size past what `DIAMOND_MAX_CALL_DEPTH` was calibrated for under
  AddressSanitizer's redzone-inflated frames (see the call-depth
  regression entry earlier in this doc — the exact same class of bug,
  now triggered from a new direction), turning `depth(5000)`-style deep
  recursion into a genuine ASan stack-overflow crash instead of the
  clean, rescuable `SystemStackError` it's supposed to raise. Fixed by
  moving the regex opcode bodies into their own `static` helper functions
  (`regexp_new_helper`/`regexp_match_helper`), so their locals live in a
  separate, transient frame that only exists while regex code is actually
  executing — not baked into every recursive `run_chunk` level the way an
  inline case body's locals are. Worth remembering for any future
  opcode whose handler needs a nontrivial local (a struct, not just a
  handful of scalars): factor it out from the start rather than adding it
  inline, since the cost of getting this wrong is invisible until
  something exercises deep recursion specifically.

- Self-hosting, Phase 0: raised every `DIAMOND_MAX_*` fixed-size limit in
  `src/vm.h` substantially (functions 64→256, code-per-function 1024→4096,
  classes 32→128, methods 32→128, constants 256→256 (unchanged — see
  below), string constants 64→256, interfaces/modules 16→32, type sets
  64→256, fields 32→64, namespace constants 64→128 —
  `DIAMOND_MAX_FUNCTION_NAME`, `DIAMOND_MAX_STRING_LENGTH`,
  `DIAMOND_MAX_UNION_TYPES`, and `DIAMOND_REGISTER_COUNT` left unchanged),
  the first step of the
  self-hosting roadmap (see `Later experiments`): a Diamond-language
  reimplementation of the compiler will need far more than 64 top-level
  functions (`src/compiler.c` alone already has ~89), so the existing
  limits were sized for ordinary Diamond programs, not a program the
  size and shape of a compiler.

  The plan for this change described it as a pure capacity increase with
  no logic changes, since these are `enum` constants sizing fixed arrays
  already embedded (not pointed to) inside `DiamondProgram` and its
  nested structs. That assumption turned out to be incomplete. Measuring
  `sizeof(DiamondProgram)` directly (rather than trusting the "no logic
  changes" characterization) found it grows from an already-nontrivial
  ~3.19MB at the old limits to ~85.9MB at the new ones — and
  `src/main.c`'s `run_source`, the function behind every single Diamond
  program execution, was declaring its `DiamondProgram` as a plain stack
  local. The ~3.19MB figure was already a known, carefully-managed
  constraint (`diamond_compile` itself uses `memset` instead of a
  compound-literal zero-init specifically to avoid a second 3MB+ stack
  temporary during nested `require`d-package compilation, and
  `src/loader.c` already heap-allocates its own `DiamondProgram` for
  exactly this reason — see the package-manifest entry earlier in this
  doc). At ~85.9MB, leaving `main.c`'s copy on the stack would have
  turned every program run into an immediate stack-overflow crash, not
  an edge case. Fixed by heap-allocating it there (`malloc`, freed at
  every exit path, matching `loader.c`'s existing precedent) and
  converting `tests/api_invalidation.c`'s equivalent stack local to
  `static` storage (mirroring `tests/fiber_run.c`'s existing convention
  for the same struct). Caught and fixed before any build or test was
  attempted against the raised limits, not after a crash.

  One test needed updating rather than the implementation: `tests/run.sh`
  asserted that composing two 20-method interfaces (40 methods total)
  overflows `DIAMOND_MAX_METHODS` and fails to compile — a correct test
  of the *old* 32-method limit that necessarily stopped being true once
  the limit became 128. Raised to two 70-method interfaces (140 total)
  to keep exercising the real overflow path at the new limit. Verified
  with `make test-all` (debug/release/sanitizer builds, all C-level test
  binaries) — all 845 `tests/run.sh` cases plus every C-level test still
  pass.

  **Follow-up correction, found while scoping Phase 1**: the initial
  Phase 0 commit raised `DIAMOND_MAX_FUNCTIONS` and `DIAMOND_MAX_CONSTANTS`
  to 512, past a hard architectural ceiling neither this round nor the
  original design ever named explicitly — every function/constant index
  is a single byte throughout the bytecode format and the structs that
  reference one (`CALL`/`CONSTANT` opcode operands are one byte;
  `DiamondMethod.function_index`/`DiamondClosure.function_index` are
  `uint8_t`). `add_constant`/the function-declaration sites already guard
  with `if (count == DIAMOND_MAX_FUNCTIONS)` before incrementing, but that
  guard only prevents exceeding the *configured* limit — at 512 it happily
  permits `count` to reach 511, and `(uint8_t)511` truncates to `255`,
  silently colliding with whatever real function or constant already
  lives at index 255 rather than erroring. `DIAMOND_MAX_STRING_CONSTANTS`
  and `DIAMOND_MAX_TYPE_SETS` were, by contrast, already correctly capped
  at exactly 256 in the same original commit (256 possible values at
  indices 0–255 is the actual ceiling a one-byte index allows, not 255) —
  which is what made the other two constants' jump to 512 a visible
  outlier worth double-checking rather than something that looked
  consistent with the rest of the change. Every other raised constant
  (classes, interfaces, modules, methods, fields, namespace constants) was
  checked against the same question and confirmed safe: all sit well
  under 256 already. Fixed by capping both back to 256 — still 4x their
  original value (64 and 256, respectively; constants was in fact already
  at 256, so it is unchanged from Phase 0's own starting point) — and
  re-verified with the same `make test-all` pass. This was caught and
  fixed before it shipped in anything beyond this one already-pushed
  commit; a second, corrective commit landed the fix rather than amending
  history.

- Self-hosting, Phase 1: `ProgramBuilder`, a native bridge letting Diamond
  code construct and run a `DiamondProgram` at runtime — the prerequisite
  for a Diamond-language compiler to produce anything executable, since
  bytecode execution doesn't care whether a `DiamondChunk` came from
  `diamond_compile` (C) or was assembled by a Diamond program at runtime.
  `ProgramBuilder.new()` is new dedicated compiler recognition (mirroring
  `Regexp.new`/`File.open`, a new `DIAMOND_OP_PROGRAM_BUILDER_NEW`
  opcode); every instance method
  (`.declare_function`/`.emit_byte`/`.add_constant`/`.add_string`/
  `.set_register_count`/`.run`) dispatches through the ordinary `INVOKE`
  opcode like any other native-kind receiver, needing no compiler changes
  at all — mirroring `compiler.c`'s own internal `emit_byte`/
  `add_constant`/`allocate_register` functions closely enough that the
  eventual Diamond-language port (Phase 3) can be a close transliteration.
  `diamond_compile`'s built-in-exception-class setup (previously inlined)
  was extracted into a shared `diamond_program_init`, so a builder-created
  program has the same immediately-instantiable `Exception`/`TypeError`/
  etc. hierarchy `diamond_compile` itself provides, without going through
  the parser at all.

  Deliberately scoped narrower than the full design sketched when this
  phase was planned: `.declare_class`/`.declare_method` don't exist yet
  (added when Phase 3's class-compiling logic actually needs them);
  `.add_constant` rejects any heap-object `DiamondValue` (matches
  `compiler.c`'s own `add_constant`, which in practice is only ever
  called with `Int`/`Float`); and `.run()` rejects a heap-object result
  and reports any failure as a plain `RuntimeError` in the calling
  program, never the original status or exception object. That last cut
  is the load-bearing one: `.run()` executes the constructed program on
  a *separate* `DiamondVm`, and a returned heap object would live in that
  VM's own object list, invisible to the calling VM's GC the moment the
  inner VM is freed — correctly transplanting a live object graph across
  two independent GC heaps is real, separate work, deferred rather than
  gotten wrong. Restricting constants and results to scalars sidesteps
  the whole problem for this round.

  Two real bugs surfaced during implementation, not just new code:
  - **Not anticipated, a second confirmed stack-overflow-guard
    regression**: mirroring the Regexp round's own "factor large locals
    out of run_chunk's switch" fix wasn't enough on the first attempt.
    Moving only the `~50KB` `DiamondVm run_vm` local (by far the largest
    single addition) into its own helper function still left `depth(5000)`
    — the existing regression test for the `DIAMOND_MAX_CALL_DEPTH` guard
    — segfaulting before that guard could trip. Root cause: at `-O0`,
    *every* local anywhere in `run_chunk`'s switch contributes to its one
    shared stack frame regardless of which branch runs, so six method-name
    flags plus each of six branches' own few pointers/integers — individually
    tiny — added up to enough combined footprint to matter, given how
    tightly `DIAMOND_MAX_CALL_DEPTH=100` was already calibrated (see the
    original call-depth regression entry above: real recursion already
    failed around depth ~150-220 with the *pre-existing* frame size, before
    any of this round's additions). Confirmed by testing the partial fix in
    isolation and watching it still crash. Fixed completely by factoring
    the *entire* `ProgramBuilder` dispatch block — all six methods, not
    just `.run()` — into its own `program_builder_invoke_helper` function,
    leaving only a handful of small locals in `run_chunk` itself. Re-verified
    `depth(5000)` passes clean under both the plain debug build and
    `make test-sanitize` afterward.
  - **Anticipated by the design, confirmed necessary by testing it
    directly**: `.run()` starts a *fresh* `run_chunk` recursion (depth 0)
    on top of its own call's C stack frame, which is itself already
    `depth` levels deep in the *calling* VM — so a program that calls
    `.run()` from inside deeply recursive Diamond code could overflow the
    real C stack well before either VM's own `DIAMOND_MAX_CALL_DEPTH`
    guard, individually, would trip. Guarded by refusing to nest past
    `depth>=10` (leaving headroom for the inner program's own full
    recursion budget within the same overall margin already verified safe
    under ASan), raising the existing, already-rescuable
    `DIAMOND_VM_STACK_OVERFLOW`/`SystemStackError` rather than crashing.
    Verified with a real 20-deep nested-`.run()` test that raises cleanly
    instead of segfaulting.

  New regression coverage: eight `tests/cases/program_builder_*` cases —
  a hand-assembled "return 42" program (the concrete verification target
  named when this phase was planned), a real `CALL` between two
  builder-declared functions (`double(5) == 10`), and one case each for
  the scope cuts and guards above (bad builder arguments, an unknown
  method, a non-scalar constant, a non-scalar `.run()` result, a failing
  constructed program, and the nesting-depth guard) — plus the
  `depth(5000)` regression already in `tests/run.sh`, which is what
  caught the stack-frame bug above in the first place. Verified with the
  same `make test-all` pass (debug/release/sanitizer builds, every
  C-level test binary) as every other round this session.

- Self-hosting, Phase 2: `selfhost/lexer.di`, a Diamond-language port of
  `src/lexer.c`'s `diamond_lexer_next` — `class Token`
  (`kind`/`start`/`length`/`line`/`column`, via `attr_reader`) and `class
  Lexer` mirroring the C `DiamondLexer` struct's own fields
  (`@source`/`@start`/`@current`/`@line`/`@column`/`@token_line`/
  `@token_column`). Token kinds are Symbols named after the C
  `DIAMOND_TOKEN_*` constants in lower\_snake\_case with the prefix
  dropped (`:left_paren`, `:identifier`, ...) — Diamond has no `enum`
  keyword, and Symbols already give readable, content-compared values
  with no interning table needed (see the Symbol entry above). Diamond
  has no `char` type, so every character comparison works on `Int`
  codepoints via `String#ord()` rather than C `char` values; a new
  `code_at(index)` helper (the one primitive the C original didn't need)
  stands in for C's implicit "index past the end reads a safe `'\0'`"
  null-terminator behavior with an explicit bounds check instead, since
  Diamond's own bounds-checked `String#[]` would otherwise raise.

  Verified with a new differential harness (`tests/lexer_diff.sh`, wired
  into `make test-lexer-diff` and `test-all`): a new C-side tool
  (`tests/lexer_dump.c`, linking only `src/lexer.c` — confirming the
  lexer's own documented independence from `compiler.c`) and a Diamond
  driver (`selfhost/lexer_dump.di`) print an identical
  `kind start length line column` line per token, and the harness diffs
  them across every one of the 806 `tests/cases/*.di` files. All 806
  matched byte-for-byte on the first full run after fixing the two
  issues below — including `selfhost/lexer.di`'s own source file,
  lexed against itself as an extra dogfooding check (not part of the
  automated suite, but confirms the port doesn't just pass on the
  narrower "ordinary program" style of the existing corpus).

  Two real, non-obvious constraints surfaced during the port, neither
  anticipated when this phase was scoped:
  - **Register-budget exhaustion, a new failure mode this session's
    prior rounds hadn't hit**: an initial `next_token()` covering every
    branch (whitespace/comment skipping, number/string/identifier/
    instance-variable dispatch, *and* the full punctuation chain) failed
    to compile at all -- "program needs too many registers". Register
    allocation is monotonic per function body and never recycled (see
    `Compiler.next_register`, `src/compiler.c`), and unlike the
    `DIAMOND_MAX_*` constants raised in Phase 0, the 256-register
    ceiling is a hard architectural limit, not a struct-sizing choice:
    register operands are single bytes throughout the bytecode format,
    the exact same constraint Phase 0's follow-up fix already found for
    function/constant indices. Fixed by splitting the punctuation chain
    into its own `scan_punctuation` method, resetting the register
    counter for a fresh function. Worth remembering for Phase 3's much
    larger parser/emitter port: a single large method covering many
    branches is a real, previously-untested way to run out of registers,
    not just a style preference.
  - **No multi-line boolean expressions**: a boolean expression split
    across lines with a trailing `&&`/`||` (e.g. a parenthesized
    condition wrapped for readability) fails to parse — newline-skipping
    only exists at the three points a bracket-delimited *list* needs it
    (after an opening bracket, after a comma, before a closing bracket —
    see the bracket-delimited-newlines entry above), not inside a general
    expression between two operands of a binary operator. Not a bug to
    fix (out of scope for this round, and arguably correct given Ruby's
    own similar restriction) — worked around by keeping each condition on
    one line, confirmed as a real, previously-undocumented parser
    constraint the C compiler itself was never tested against, since
    every existing `tests/cases/*.di` file happens not to need it.

- Self-hosting, Phase 3 sub-phase 1 ("expression evaluator core"):
  `selfhost/parser.di`, a Diamond-language port of the parts of
  `src/compiler.c`'s single-pass parser needed for literals, arithmetic/
  comparison/logical expressions, local variables, and `if`/`while`/
  `loop`/`break` — the first, smallest vertical slice of the parser, per
  the self-hosting roadmap plan's suggested internal sequencing. `class
  Parser` mirrors `Compiler`'s own fields it actually needs at this stage
  (source/lexer/current/previous token, a locals list, a loop-context
  stack, a running register counter, failure state) and emits real
  bytecode through Phase 1's `ProgramBuilder` bridge, closely enough that
  the port is a transliteration rather than a redesign — `emit_jump`/
  `patch_jump`/`allocate_register`/`add_constant`/`add_string` are
  near-identical to their C namesakes. Opcode/precedence values are
  declared as `module Opcode`/`module Precedence` namespace constants
  (not plain top-level locals, which turn out to be invisible from
  inside a class method's own separate function/frame — a genuine, only
  now-relevant Diamond scoping rule this session's earlier top-level-only
  constant usage never had to confront) — referenced via qualified
  `Opcode::NAME` lookup, which works from any lexical scope regardless of
  the referencing code's own module/class nesting.

  Required one new, small addition to the Phase 1 bridge:
  `ProgramBuilder#patch_byte(function_index, offset, byte)`, overwriting
  an already-emitted byte rather than appending one — needed because
  `if`/`while`/`loop`/`break`/`&&`/`||` all backpatch a forward jump's
  real target only once it's known, exactly like `compiler.c`'s own
  `patch_jump` directly rewrites `function->code[operand]`. `emit_byte`
  alone can't express this (append-only).

  Deliberately narrower than the eventual full parser, matching this
  sub-phase's own scope in the plan: no functions/closures/calls,
  classes/methods/`super`, interfaces/generics/narrowing, or exceptions/
  modules/`require` (each a separate later sub-phase); no compile-time
  `_INT` opcode quickening (every arithmetic/comparison opcode emitted
  is the generic form — always correct, just not compile-time
  specialized, since the VM's own opt-in runtime quickening can still
  apply); no string interpolation (only `\n`/`\t`/`\r`/`\"`/`\\`/`\#`
  escape decoding); no `next`/`redo` or postfix `if`/`unless` modifiers.

  Verified with a new differential harness (`tests/parser_diff.sh`,
  `make test-parser-diff`, wired into `test-all`) comparing the real
  compiler's output against `selfhost/parser_run.di` (parse, build, and
  run a file's source through `selfhost/parser.di`) across 19
  hand-curated `tests/parser_cases/*.di` files — unlike Phase 2's lexer
  harness, this can't reuse the full `tests/cases/*` corpus, since almost
  all of it exercises syntax well beyond this sub-phase's deliberately
  narrow grammar. All 19 match, covering arithmetic/float/comparison/
  logical precedence and short-circuiting, locals and shadowing,
  `if`/`elsif`/`else`/`unless` (including as an assignment's RHS on one
  line), `while`/`until`/`loop`+`break` (including multiple break sites
  and a break value), string literal escaping (verified indirectly via
  equality, since `ProgramBuilder#run`'s scalar-only result restriction
  from Phase 1 means a bare String can't be the final observed result
  yet), and underscored integer literals.

  One real, previously-undocumented parser constraint surfaced building
  this (Phase 2's own "no multi-line boolean expressions" note
  undersold how often it actually bites): every multi-line `&&`/`||`
  chain written the natural way while porting `token_precedence`/
  `parse_precedence`/`at_block_end?`/`compile_break`'s own conditions
  had to be collapsed onto one line. Not fixed (out of scope for this
  round, see Phase 2's entry above) — but confirms this is a real,
  recurring cost for hand-written Diamond code with non-trivial boolean
  logic, not a one-off in the lexer port.

- Self-hosting, Phase 3 sub-phase 2: `selfhost/parser.di` gained
  top-level named functions (purely positional parameters, no defaults)
  and direct calls, including self-recursion (a function's own entry is
  registered in the parser's own function table before its body is
  compiled, exactly mirroring `compiler.c`'s ordering, so mutual
  recursion between two functions has the same "must already be
  declared" constraint the real compiler has) — plus `puts`/`print`,
  finally closing the gap flagged when sub-phase 1 landed: string
  literals can now be verified directly through printed output rather
  than only equality comparison.

  Closures (nested `def`) were deliberately split out of this round
  despite the plan's phase header bundling "functions, closures, calls"
  together — captures are their own self-contained mechanism (`BOX_LOCAL`/
  `GET_CAPTURE`/`SET_CAPTURE`/`GET_CELL`/`SET_CELL`/`CLOSURE`/
  `CALL_CLOSURE`) with no bearing on plain top-level functions working
  correctly first; a nested `def` is a clear, explicit compile error
  here ("nested function definitions are not yet supported"), not a
  silent miscompile.

  Needed one small addition beyond what sub-phase 1 already required
  from the Phase 1 bridge: `emit_byte`/`add_constant`/`add_string`/
  `patch_byte` all previously hardcoded `-1` (the ProgramBuilder entry
  function) — no new C code, since `ProgramBuilder`'s methods already
  took a function index; `Parser` just needed to route every emission
  call through a `@current_function_index` field that `compile_definition`
  switches to the new function (and restores afterward) instead of
  hardcoding the entry function everywhere.

  One real, subtle correctness gap surfaced during implementation, not
  from the differential harness: an early draft only recognized a
  function call when `find_function` already succeeded, falling back to
  `parse_identifier`'s "undefined local variable" message otherwise —
  producing a misleading diagnostic for `undefined_fn(1)` (a clearly
  attempted call, not a bare variable reference). `compiler.c`'s real
  `parse_name` unconditionally treats an identifier followed by `(` as a
  call attempt and lets `parse_call` itself fail with "undefined
  function"; restructured to match that exactly, resolving the function
  lookup *inside* `compile_call` rather than before deciding whether to
  call it at all.

  New regression coverage: eight more `tests/parser_cases/*.di` cases
  (direct calls, recursion, mutual self-recursion via `fib`, multiple
  independent functions, local-variable isolation between a function's
  own parameters and the outer script's locals of the same name,
  `puts`/`print` with string arguments including the no-newline `print`
  form) — bringing the differential harness to 27 cases, all matching.
  Verified with the same `make test-all` pass as every other round,
  plus the parser differential harness re-run directly under
  `-fsanitize=address,undefined` (clean, no leaks) given the new
  `declare_function`/`CALL` interaction this round exercises for the
  first time.

- Self-hosting, Phase 3 sub-phase 2 follow-up: closures. `selfhost/
  parser.di`'s `compile_definition` now handles a nested `def` (one
  written directly inside a top-level function's own body) as a
  closure: it becomes a local variable named after the function, holding
  a `Closure` value, called through `CALL_CLOSURE` like any other
  closure-valued local — mirroring `compiler.c`'s own distinction
  between a top-level `def` (found later by name, called with `CALL`)
  and a nested one (`at_top_level`, computed from a new
  `@function_nesting_depth` counter). Every local in scope at the moment
  a nested `def` is encountered becomes a capture candidate
  unconditionally, whether or not the nested body actually references
  it — mirroring `compiler.c`'s own eager design exactly, not a
  reference-counting optimization. `GET_CAPTURE_CELL` loads each capture
  into the nested body as an ordinary (but `captured`-flagged) local, so
  the *existing* `find_local`/`read_local`/`compile_assignment` machinery
  handles reads and writes with no special-casing; only `compile_assignment`
  needed a small addition (a captured local's write goes through
  `SET_CELL` on its cell register rather than a plain `MOVE`). Back in
  the enclosing scope, `BOX_LOCAL` converts each captured local's
  register from a plain value to a `Cell` in place (idempotent, matching
  `compiler.c`'s own "always emit, even if a sibling branch already
  boxed it" reasoning from earlier this session — see the nested-`def`-
  in-sibling-branches entry above), and marks that local's own
  `@locals` entry `captured` too, so every subsequent read/write of it
  in the *enclosing* function also routes through `GET_CELL`/`SET_CELL`
  from that point on — the same "once boxed, always boxed" semantics
  `compiler.c` has.

  `@locals` entries grew a third field (`[name, register, captured]`,
  up from `[name, register]`) and `find_local` now returns the whole
  entry (or `nil`) rather than a bare register, since callers need
  `captured` to decide between a direct read/`MOVE` and a
  `GET_CELL`/`SET_CELL` unwrap.

  Deliberately narrower than `compiler.c`'s own general mechanism:
  exactly one level of function nesting is supported (a `def` inside a
  top-level `def`'s body), not arbitrary depth — `compiler.c`'s own
  `enclosing_locals`-walk-with-dedup logic (letting a doubly-nested
  closure transitively reach two levels up) was skipped as real,
  separate complexity with no bearing on getting one level of nesting
  correct first. A `def` written inside an already-nested `def` is a
  clear compile error ("only one level of function nesting is
  supported"), not a silent miscompile or a crash.

  The first, single-method implementation of `compile_definition` hit
  the same register-budget wall Phase 2's lexer port already found and
  documented — unsurprising in hindsight (more local variables than any
  method written so far in this port) but a useful confirmation that the
  lesson generalizes: split into `compile_definition` (orchestration),
  `parse_parameter_names`, `compile_function_body` (the switch-state/
  compile-body/restore-state work shared identically by both a top-level
  function and a nested closure), and `emit_closure` (the `BOX_LOCAL`+
  `CLOSURE` emission, only reached for the nested case). Any method of
  comparable size should be split from the start, not after hitting the
  limit.

  New regression coverage: five more `tests/parser_cases/*.di` closure
  cases (a classic counter/adder pair, mutation of a captured variable
  visible after the closure returns control to its definer, multiple
  captures combined in one expression, and a parameter shadowing a
  captured name of the same spelling) bring the differential harness to
  32 cases, all matching. Also hand-verified two negative cases directly
  against the real compiler (confirming, not just asserting, that they
  *should* fail): a top-level `def` can't see the top-level script's own
  locals (`compiler.c` rejects it too — top-level functions never
  capture, only nested ones do), and a doubly-nested `def` correctly
  hits this round's own one-level-only scope cut. Verified with the same
  `make test-all` pass as every other round, plus the parser
  differential harness re-run directly under
  `-fsanitize=address,undefined` (clean, no leaks) given closures
  exercise new heap allocation and GC-marking paths (`Closure`/`Cell`
  objects) for the first time in this port.

- Self-hosting, Phase 3 sub-phase 3 (first slice): basic classes.
  `selfhost/parser.di` gained `class Name ... end` (methods, `@ivar`
  read/write, `self`, `ClassName.new(args)`, and `receiver.method(args)`
  dispatch through the existing postfix-`.` loop in `parse_precedence`)
  — deliberately without inheritance/`super` yet, split out the same way
  closures were split from plain functions in sub-phase 2.

  Required three new `ProgramBuilder` methods — the class-declaration
  surface Phase 1's own design note flagged as deferred until a
  class-compiling sub-phase actually needed it:
  - `declare_class(name, superclass_index)` → class index (`-1` for no
    superclass; superclass support is included in the C method even
    though this round's Diamond-side `compile_class` doesn't use it
    yet, since inheritance is purely a *frontend* addition once this
    lands — no further bridge work needed for the sub-phase 3 follow-up).
  - `declare_field(class_index, name)` → field index, idempotent (a
    second call with the same name returns the existing index rather
    than adding a duplicate) — mirroring `field_index`'s own
    find-or-create behavior in `compiler.c`, used identically for both
    a field's first read and its first write, so callers never need to
    ask "does this field already exist" themselves.
  - `declare_method(class_index, name, function_index, arity,
    required_arity, is_private)`.

  Both `declare_class` and `declare_field` recompute the owning class's
  `shapes[]` array on every change (a new `program_builder_recompute_shapes`
  helper, mirroring the loop `diamond_compile` itself runs once, over
  every class, right after compilation finishes) — necessary because a
  `ProgramBuilder`-built program never goes through `diamond_compile` at
  all, so nothing else would ever populate shapes for a class declared
  this way.

  `GET_IVAR`/`SET_IVAR`'s receiver operand is an ordinary register
  operand, not implicitly `self` at the opcode level — `self` being
  register 0 is purely a *convention* `compile_method` establishes by
  always allocating it first, before any user-declared parameter,
  exactly mirroring `compile_definition`'s own class/module branch.

  Two design decisions confirmed by directly testing against the real
  compiler rather than assumed: a nested `def` inside a function body
  compiling a `class` is accepted by `compiler.c` (no top-level
  restriction on class declarations in the real language) — this
  round's own top-level-only restriction is a genuine, deliberate
  narrowing beyond what the real compiler requires, not a mirroring of
  an existing constraint (documented as such in `compile_class`'s own
  comment). Method-name duplicate detection is handled in the Parser's
  own `@current_class_method_names` tracking (checked *before* calling
  `declare_method`) rather than relying on `declare_method`'s own
  VM-level duplicate rejection, so a duplicate method surfaces through
  this parser's own `fail()`/`error_message()` mechanism instead of an
  uncaught exception escaping mid-compile.

  A genuinely new failure mode surfaced while iterating on this round,
  worth remembering going forward: hand-assembling bytecode
  instruction-by-instruction as literal top-level Diamond statements
  (one `builder.emit_byte(...)` call per source line, as every earlier
  round's scratch verification scripts did) burns through the *real*
  compiler's own 256-register budget fast, since `compiler.c`'s
  register allocator is monotonic at every scope including the
  top level, never recycled — confirmed by a ~40-line hand-assembled
  scratch script failing to compile at all. Fixed by wrapping repeated
  emission in an ordinary Diamond function taking an array of bytes,
  called in a loop, so the *function's* own register frame absorbs the
  repetition instead of the top-level script's. Also caught, again, the
  same register-budget wall this port has now hit three times running
  (Phase 2's lexer, sub-phase 2's `compile_definition`, and this round):
  the first version of `compile_class` combined with method-compiling
  logic in one method and needed splitting into `compile_class`/
  `compile_method`/`current_class_has_method?`/`compile_method_body`
  from the start.

  New regression coverage: five more `tests/parser_cases/class_*.di`
  cases (fields plus a self-call between two methods, a mutating
  method, a class relying entirely on `initialize`'s implicit `nil`
  defaults for state it never explicitly sets before first use, two
  independent instances confirming field storage isn't shared, and one
  method calling a sibling method on `self`) bring the differential
  harness to 37 cases, all matching. Verified with the same `make
  test-all` pass as every other round, plus the parser differential
  harness re-run directly under `-fsanitize=address,undefined` (clean,
  no leaks) given classes exercise new instance-allocation and
  shape-transition paths for the first time in this port.

- Self-hosting, Phase 3 sub-phase 3 follow-up: inheritance (`class Sub <
  Base`) and `super(...)`. Needed no new C bridge work at all — the
  previous round's `declare_class(name, superclass_index)` already
  copied superclass fields and set the class link, unused until now;
  this round was purely `compile_class` gaining `< Superclass` parsing
  (a new `parse_optional_superclass` helper) and a new `parse_super`
  emitting the `SUPER` opcode. `SUPER`'s own operand shape mirrors
  `compiler.c`'s own `parse_super` exactly: the opcode's `class_index`
  operand is the *current* class (not the superclass directly) and the
  receiver is implicitly register 0 (`self`, never an explicit operand)
  — the VM walks `owner->superclass` and looks up the same-named method
  there itself at runtime, so the parser only needs to track "does the
  current class have a superclass at all" (a new
  `@current_class_superclass_index`) for a friendly compile-time
  rejection, not the superclass's full identity. `super(...)` always
  calls the superclass's version of *this same* method — a new
  `@current_method_name`, set for the duration of `compile_method`,
  supplies the name `SUPER` needs.

  Inherited (non-overridden) method dispatch needed zero parser-side
  work: `receiver.method(...)` already compiles to a plain `INVOKE`
  regardless of whether `method` is declared on the receiver's own
  class or inherited from a superclass — the superclass-chain walk is
  entirely `lookup_method`'s own existing runtime behavior, already
  correct and already tested before this port touched it at all.

  New regression coverage: five more `tests/parser_cases/class_*.di`
  cases (an overridden method, an inherited method used unchanged,
  `super(...)` from a constructor, `super()` from an ordinary
  non-constructor method, and a three-level inheritance chain each
  level calling `super()`) bring the differential harness to 42 cases,
  all matching. Verified with the same `make test-all` pass as every
  other round.

- Self-hosting, Phase 3 sub-phase 2's remaining gaps: explicit `return`
  and keyword arguments, closing sub-phase 2 out entirely. Neither
  needed any C bridge changes — both compile purely to opcodes the port
  already emits (`RETURN`/`CALL`/`MOVE`).

  `return` turned out simpler than `if`/`while`/`loop`/`break` needed to
  be: `RETURN` halts `run_chunk` unconditionally the instant it
  executes, wherever it is in the bytecode, so a mid-body `return`
  (nested inside an `if`, say) needs no jump/patch bookkeeping at all —
  unlike every other control-flow construct this port has added so far.
  Scoped the same documented way `break` already was: no postfix
  `if`/`unless` modifier form (`return if cond` isn't supported, and
  would misparse as returning an `if`-expression's value, the same
  divergence `break if cond` already has).

  Keyword arguments (`f(y: 2, x: 1)`, or positional-then-keyword mixed)
  are direct-call-only, matching `compiler.c`'s own scoping exactly:
  only a direct call to an already-declared top-level function has
  compile-time, non-polymorphic access to the callee's exact parameter
  names, unlike a closure call (target unknown until runtime) or an
  `INVOKE` (resolved by the receiver's runtime class). `@functions`
  entries grew a fourth field (the function's `parameter_names`,
  already available in `compile_definition` but previously discarded)
  so a call site can resolve a keyword to its slot. Simpler than
  `compiler.c`'s own version in exactly one way: since this port has no
  default parameter values at all, every declared slot must always be
  filled by the call site — there's no partial-call case needing a
  separate `required_arity` bound the way the real compiler's version
  has, so `arity` alone is both bounds here.

  New regression coverage: four more `tests/parser_cases/*.di` cases
  (an early `return` from inside a nested `if`, a bare `return` inside
  a class method, and two keyword-argument call shapes — fully
  reordered and positional-then-keyword mixed) bring the differential
  harness to 46 cases, all matching. Also hand-verified four error
  cases directly (an unknown keyword name, a positional argument
  following a keyword one, a missing required slot, and supplying the
  same slot twice) — all produce the expected `fail()`/`error_message()`
  rejection. Verified with the same `make test-all` pass as every other
  round. This closes out Phase 3 sub-phase 2 (functions, closures,
  calls) exactly as scoped by the plan.

- Self-hosting, Phase 3 sub-phase 4 (first slice): scalar gradual
  typing. `selfhost/parser.di` gained `param: Type`/`-> Type`
  annotations (`Int`/`Float`/`String`/`Bool`/`Nil`, or a declared class
  name) on functions, closures, and methods, checked at every return
  path (both the implicit final-expression return and every explicit
  `return` statement) via a new `CHECK_TYPE` emission — deliberately
  without unions, `Array[T]`/`Hash[K,V]`, `Callable`, interfaces,
  generics, or narrowing yet, each its own separate later slice of this
  sub-phase.

  Required one new `ProgramBuilder` method: `declare_type_set(function_index,
  type_id)` → type-set index, creating a single-member `DiamondTypeSet`
  (no union/array/hash/callable support in the bridge method itself
  either — a natural, separate future extension, the same "add it when
  a sub-phase actually needs it" precedent `declare_class`/
  `declare_field`/`declare_method` already set in sub-phase 3).

  Simpler than `compiler.c`'s own `emit_type_check` in one deliberate
  way: since this port tracks no compile-time type information at all
  (no `known_types`/`known_type_sets` — the same `_INT`-quickening
  scope cut from sub-phase 1, generalized), every type-annotated
  parameter or return unconditionally emits a real `CHECK_TYPE`
  instruction rather than sometimes eliding it when the value's type is
  already known statically. Always correct, just not optimized —
  consistent with every other place this port has made the same
  trade-off.

  A correctness gap surfaced and was fixed while implementing this, not
  found by the differential harness: an early version only checked the
  return type at the implicit final-expression return path, missing
  every explicit `return` statement entirely (confirmed against
  `compiler.c`'s own `compile_return`, which checks
  `current_return_type` on *every* return path, not just the implicit
  one). Fixed by threading a new `@current_return_type` field through
  `compile_function_body`/`compile_method_body` (save/set/restore, the
  same pattern already used for `@current_method_name`/
  `@current_class_index`) so `compile_return` can check it too.

  New regression coverage: four more `tests/parser_cases/*.di` cases
  (a function with `Int` parameter and return types, an early `return`
  path also getting return-type-checked, a class constructor/method
  pair with typed parameters, and mixed `Float`/`Int` parameter types)
  bring the differential harness to 50 cases, all matching. Also
  hand-verified three type-violation cases directly against the real
  compiler (a bad argument type, a bad implicit return, and a bad
  explicit `return`) — all three produce the identical `"expected X,
  got Y"` error message. Verified with the same `make test-all` pass as
  every other round, plus the parser differential harness re-run
  directly under `-fsanitize=address,undefined` (clean, no leaks).

- Self-hosting, Phase 3 sub-phase 4 (second slice): union annotations.
  The Diamond-language parser now accepts up to eight pipe-separated
  primitive or nominal class types in function, closure, and method
  parameter and return annotations. The `ProgramBuilder` bridge now
  constructs a complete scalar/nominal `DiamondTypeSet` from an array of
  resolved type IDs, retaining the same runtime `CHECK_TYPE` path used by
  the first slice. Duplicate members and over-wide unions are rejected
  during parsing. A parameter-and-return union differential case brings
  the parser harness to 51 cases, all matching the C compiler.

- Self-hosting, Phase 3 sub-phase 4 (third slice): persistent collection
  contracts. Recursive annotation trees now represent `Array[Element]` and
  `Hash[Key, Value]`, including nested unions and collections. The
  `ProgramBuilder` bridge accepts member descriptors referencing previously
  declared nested type sets, producing the same persistent runtime contracts
  used by the C compiler. A nested collection-signature differential case
  brings the parser harness to 52 cases, all matching.

- Self-hosting, Phase 3 sub-phase 4 (fourth slice): callable contracts.
  Arity-only, return-typed, and fully parameter-typed `Callable` annotations
  now compile through recursive member descriptors. `ProgramBuilder` also
  records parameter and return type-set metadata on generated functions, so
  structural callable checks see the same signatures as C-compiled code in
  addition to the emitted entry/return guards. An invoked typed-closure case
  brings the parser harness to 53 cases, all matching.

- Self-hosting, Phase 3 sub-phase 4 (fifth slice): structural interfaces.
  Top-level interface declarations with typed method signatures now register
  through `ProgramBuilder`, interface names resolve in recursive annotations,
  and generated class method metadata participates in the VM's existing
  structural conformance checks. An interface accepted by an independently
  declared conforming class brings the parser harness to 54 cases, all
  matching. Interface inheritance remains outside this initial slice.

- Self-hosting, Phase 3 sub-phase 4 (sixth slice): inferred generics. Function
  declarations now accept up to eight scoped type variables, recursive type
  annotations resolve them to VM variable IDs, and `ProgramBuilder` retains
  their names/count so the existing runtime inference machinery binds argument
  types through checked returns. Scalar identity inference is exercised at
  runtime, with a persistent `Array[T]` signature covering recursive metadata;
  the parser harness now has 55 matching cases. Explicit generic call arguments
  remain a later extension.

- Self-hosting, Phase 3 sub-phase 4 (seventh slice): interface inheritance.
  Interface declarations now accept one or more previously declared bases;
  `ProgramBuilder` copies inherited structural requirements with duplicate and
  capacity checks. A derived-interface contract satisfied by a class providing
  both inherited and direct methods brings the parser harness to 56 cases.

- Self-hosting, Phase 3 sub-phase 4 (eighth slice): explicit generic calls.
  Direct calls now parse up to eight recursive type arguments, enforce the
  callee's generic arity, declare the specializations in the caller's type-set
  table, and emit `CALL_TYPED`; omitted arguments retain runtime inference.
  Two-variable calls with different specializations bring the differential
  harness to 57 cases.

- Self-hosting, Phase 3 sub-phase 4 (ninth slice): non-throwing `is`
  predicates. Primitive, nominal, and structural type names now emit the VM's
  `IS_TYPE` operation at equality precedence, including membership tests over
  union-annotated values and the same unbound-generic rejection as the C
  compiler. A union predicate case brings the harness to 58 cases.

- Self-hosting, Phase 3 sub-phase 4 (tenth slice): nil-sensitive branch facts.
  The port now retains declared parameter annotations and exact literal facts;
  equality against `nil` splits a two-way union across `if` branches, allowing
  a proven explicit return to reuse its declared type set without emitting a
  redundant runtime guard. Facts are restored conservatively at the join. A
  nil-or-integer early-return case brings the harness to 59 cases.

- Self-hosting, Phase 3 sub-phase 4 (eleventh slice): `is`-driven branch facts.
  A tested primitive, nominal, or structural type becomes the true-branch fact;
  when removing it leaves one union member, that complementary type becomes the
  false-branch fact. Both are scoped to their branch and discarded at the join.
  A nominal `Dog | Cat` return-narrowing case brings the harness to 60 cases.

- Self-hosting, Phase 3 sub-phase 4 (twelfth slice): local assignment facts and
  joins. Exact scalar facts now follow local creation and reassignment, while
  branch-local writes are discarded at the join in favor of the pre-branch
  fact unless both paths can safely reuse it. A two-branch integer assignment
  feeding an explicit typed return brings the harness to 61 cases.

- Self-hosting, Phase 3 sub-phase 4 (thirteenth slice): direct-call result
  facts. A non-generic function's single declared return member is now attached
  to the destination register and follows subsequent local assignment, while
  generic results remain conservatively runtime-bound. A typed forwarding call
  brings the parser harness to 62 cases.

- Self-hosting, Phase 3 sub-phase 4 (fourteenth slice): polarity-aware
  narrowing for `unless`. The inverted branch receives the condition's false
  fact and its `else` receives the true fact, sharing a fact-application helper
  that also keeps `parse_if` within the language's register ceiling. A nilable
  unwrap case brings the harness to 63 cases.

- Self-hosting, Phase 3 sub-phase 4 (fifteenth slice): nil-inequality facts.
  Narrowing records whether `Nil` belongs to the condition's true or false
  side, so both `== nil` and `!= nil` compose correctly with `if` and `unless`.
  A non-nil true-branch return brings the harness to 64 cases.

- Self-hosting, Phase 3 sub-phase 4 (sixteenth slice): declared-contract alias
  propagation. Assigning a typed local to another local now carries both its
  current exact fact and its full declared union, preserving later `is`/nil
  narrowing without treating unrelated reassignment as permanently narrowed.
  A nominal union alias case brings the parser harness to 65 cases.

- Self-hosting, Phase 3 sub-phase 4 (seventeenth slice): declared call-result
  contracts. Results of non-generic direct calls now retain the callee's full
  declared return union in addition to an exact single-member fact, allowing
  immediate `is` and nil narrowing before or after local aliasing. A nominal
  union-returning call brings the parser harness to 66 cases.

- Self-hosting, Phase 3 sub-phase 4 (eighteenth slice): constructor result
  facts. `Class.new(...)` destinations now carry their exact nominal class type,
  allowing constructor values and their local aliases to satisfy matching
  returns without a redundant runtime guard. A direct nominal constructor
  return brings the parser harness to 67 cases.

- Self-hosting, Phase 3 sub-phase 4 (nineteenth slice): exact conditional
  result joins. An `if`/`unless` expression now retains an exact destination
  fact when every result path has the same type, including nested `elsif`
  expressions and the implicit nil path. An integer-valued conditional brings
  the parser harness to 68 cases.

- Self-hosting, Phase 3 sub-phase 4 (twentieth slice): declared conditional
  result joins. When both branches produce values carrying the same declared
  contract, the `if`/`unless` destination now retains that full annotation for
  subsequent nil or `is` narrowing. A union-valued conditional brings the
  parser harness to 69 cases.

- Self-hosting, Phase 3 sub-phase 4 (twenty-first slice): matching local fact
  joins. Existing uncaptured locals assigned the same exact type on every
  branch now retain that type after the conditional, even when it differs from
  the incoming fact; divergent and captured values remain conservative. A
  Boolean-to-integer two-branch reassignment brings the parser harness to 70
  cases.

- Self-hosting, Phase 3 sub-phase 5 (thirteenth slice): rescue-else result
  facts. When `else` is present, normal-path fact and contract joins now use
  the replacing else value rather than the protected body's discarded result.
  A typed else result brings the parser harness to 98 cases.

- Self-hosting, Phase 3 sub-phase 5 (fourteenth slice): negative parser
  differential coverage. The harness now requires both the native and
  self-hosted compilers to reject curated invalid programs with the same
  diagnostic fragment. Bare raise seeds the error corpus with one case.

- Self-hosting, Phase 3 sub-phase 5 (fifteenth slice): duplicate rescue-filter
  validation. Repeated runtime type IDs within one filter are now rejected
  before handler emission, with a native/self-hosted differential error case.

- Self-hosting, Phase 3 sub-phase 5 (sixteenth slice): generic rescue-filter
  validation. Unbound function type variables are now rejected as runtime
  rescue filters, matching the native compiler and adding a third negative
  differential case.

- Self-hosting, Phase 3 sub-phase 5 (seventeenth slice): rescue-filter capacity
  coverage. A ninth type in one clause is now exercised through the negative
  differential harness, locking the VM handler's eight-type limit across both
  compilers.

- Self-hosting, Phase 3 sub-phase 5 (eighteenth slice): retry-context error
  coverage. A top-level `retry` must be rejected by both compilers, locking the
  scoped retry-target invariant into the negative differential harness.

- Self-hosting, Phase 3 sub-phase 5 (nineteenth slice): begin-clause error
  coverage. A `begin` expression without either `rescue` or `ensure` must be
  rejected by both compilers through the negative differential harness.

- Self-hosting, Phase 3 sub-phase 5 (twentieth slice): parser scope
  documentation. The parser and differential-harness headers now describe
  the implemented sub-phases and positive/negative corpora without stale
  claims that landed functions, classes, modules, and exceptions are absent.

- Self-hosting, Phase 3 sub-phase 5 (twenty-first slice): loop-control context
  coverage. A top-level `break` must be rejected by both compilers, locking
  the loop-target invariant into the negative differential harness.

- Self-hosting, Phase 3 sub-phase 5 (twenty-second slice): return-context
  coverage. A top-level `return` must be rejected by both compilers, locking
  the active-function invariant into the negative differential harness.

- Self-hosting, Phase 3 sub-phase 5 (twenty-third slice): receiver-context
  coverage. A top-level `self` must be rejected by both compilers, locking
  the active-method invariant into the negative differential harness.

- Self-hosting, Phase 3 sub-phase 5 (twenty-fourth slice): super-context
  coverage. A top-level `super` call must be rejected by both compilers,
  locking superclass dispatch to active method bodies.

- Self-hosting, Phase 3 sub-phase 5 (twenty-fifth slice): instance-variable
  context coverage. A top-level instance-variable read must be rejected by
  both compilers, locking receiver storage access to active method bodies.

- Self-hosting, Phase 3 sub-phase 5 (twenty-sixth slice): instance-variable
  assignment context coverage. A top-level instance-variable write must be
  rejected by both compilers as well as the corresponding read.

- Self-hosting, Phase 3 sub-phase 5 (twenty-seventh slice): missing rescue-type
  coverage. A filter colon without a following type must produce the same
  explicit rejection in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (twenty-eighth slice): missing predicate
  type coverage. An `is` expression without a following type must be rejected
  identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (twenty-ninth slice): unterminated array
  coverage. A missing closing bracket in an array literal must produce the
  same explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (thirtieth slice): malformed hash-entry
  coverage. A hash key without its separating colon must be rejected
  identically by the native and self-hosted parsers; failed hash parsing now
  stops before lowering partially populated key/value arrays.

- Self-hosting, Phase 3 sub-phase 5 (thirty-first slice): unterminated hash
  coverage. A missing closing brace in a hash literal must produce the same
  explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (thirty-second slice): unterminated index
  coverage. An indexed read missing its closing bracket must be rejected
  identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (thirty-third slice): unterminated grouping
  coverage. A parenthesized expression missing its closing delimiter must
  produce the same explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (thirty-fourth slice): unterminated call
  coverage. A direct-call argument list missing its closing parenthesis must
  be rejected identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (thirty-fifth slice): unterminated method
  call coverage. A receiver invocation missing its closing parenthesis must
  produce the same explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (thirty-sixth slice): missing member-name
  coverage. A receiver followed by `.` without a method name must be rejected
  identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (thirty-seventh slice): missing class-name
  coverage. A `class` declaration without an identifier must produce the same
  explicit diagnostic in both compilers; the self-hosted wording now matches
  the native compiler's `expected valid class name` contract.

- Self-hosting, Phase 3 sub-phase 5 (thirty-eighth slice): missing function-name
  coverage. A `def` declaration without an identifier must be rejected
  identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (thirty-ninth slice): missing function
  parameter opener coverage. A named `def` without `(` must produce the same
  explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (fortieth slice): unterminated function
  parameters coverage. A `def` parameter list missing `)` must be rejected
  identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (forty-first slice): unterminated function
  body coverage. A `def` body missing its closing `end` must produce the same
  explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (forty-second slice): unterminated class
  body coverage. A `class` body missing its closing `end` must be rejected
  identically by the native and self-hosted parsers; self-hosted class-member
  failures now use the native `method definition or include` contract.

- Self-hosting, Phase 3 sub-phase 5 (forty-third slice): unterminated
  conditional coverage. An `if` expression missing its closing `end` must
  produce the same explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (forty-fourth slice): unterminated loop
  coverage. A `while` expression missing its closing `end` must be rejected
  identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (forty-fifth slice): unterminated exception
  block coverage. A rescued `begin` expression missing its closing `end` must
  produce the same explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (forty-sixth slice): missing interface-name
  coverage. An `interface` declaration without an identifier must be rejected
  identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (forty-seventh slice): missing base
  interface coverage. An inheritance `<` without a following interface name
  must produce the same explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (forty-eighth slice): missing interface
  method-name coverage. An interface `def` without an identifier must be
  rejected identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (forty-ninth slice): missing interface
  parameter opener coverage. A named interface method without `(` must
  produce the same explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (fiftieth slice): unterminated interface
  parameters coverage. An interface method parameter list missing `)` must be
  rejected identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (fifty-first slice): missing superclass
  coverage. A class inheritance `<` without a following superclass name must
  produce the same explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (fifty-second slice): missing class-method
  name coverage. A class `def` without an identifier must be rejected
  identically by the native and self-hosted parsers; self-hosted method
  declarations now use the native `function name after def` wording.

- Self-hosting, Phase 3 sub-phase 5 (fifty-third slice): missing class-method
  parameter opener coverage. A named class method without `(` must produce
  the same explicit diagnostic in both compilers; self-hosted declarations
  now use the native `after function name` wording.

- Self-hosting, Phase 3 sub-phase 5 (fifty-fourth slice): unterminated
  class-method parameters coverage. A method parameter list missing `)` must
  be rejected identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (fifty-fifth slice): unterminated method
  body coverage. A class method body missing its closing `end` must produce
  the same explicit diagnostic in both compilers; self-hosted methods now use
  the native shared `function body` wording.

- Self-hosting, Phase 3 sub-phase 5 (fifty-sixth slice): missing generic
  parameter coverage. An empty generic parameter slot must be rejected
  identically by the native and self-hosted parsers; self-hosted declarations
  now use the native `generic type parameter` terminology.

- Self-hosting, Phase 3 sub-phase 5 (fifty-seventh slice): duplicate generic
  parameter coverage. Repeating a generic parameter name must produce the
  same explicit diagnostic in both compilers, using the native parameter
  terminology throughout the self-hosted declaration parser.

- Self-hosting, Phase 3 sub-phase 5 (fifty-eighth slice): unterminated generic
  parameters coverage. A generic parameter list missing `]` must be rejected
  identically by the native and self-hosted parsers, completing consistent
  native `generic type parameters` terminology on declaration errors.

- Self-hosting, Phase 3 sub-phase 5 (fifty-ninth slice): missing parameter-type
  coverage. A parameter annotation colon without a following type must be
  rejected identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (sixtieth slice): unterminated callable
  parameters coverage. A `Callable` parameter-type list missing `]` must
  produce the same explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (sixty-first slice): unterminated
  collection-type coverage. A collection annotation missing its closing `]`
  must be rejected identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (sixty-second slice): malformed hash-type
  coverage. A `Hash` annotation missing the comma between key and value types
  must produce the same explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (sixty-third slice): invalid scalar type
  arguments coverage. Applying collection-style arguments to a scalar type
  must be rejected identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (sixty-fourth slice): duplicate union-type
  coverage. Repeating a member in a union annotation must produce the same
  explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (sixty-fifth slice): union capacity
  coverage. An annotation exceeding eight union members must be rejected
  identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (sixty-sixth slice): missing callable
  shape coverage. A `Callable` annotation without an arity or parameter list
  must produce the same explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (sixty-seventh slice): callable arity
  capacity coverage. A numeric `Callable` arity above sixteen must be rejected
  identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (sixty-eighth slice): callable parameter
  capacity coverage. An explicit `Callable` type list above sixteen parameters
  must produce the same explicit diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (sixty-ninth slice): unterminated generic
  call coverage. An explicit generic invocation missing `]` must be rejected
  identically by the native and self-hosted parsers.

- Self-hosting, Phase 3 sub-phase 5 (seventieth slice): generic-call type
  argument arity coverage. An explicit invocation supplying the wrong number
  of generic arguments must produce the same diagnostic in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (seventy-first slice): ordered multiple
  rescue clauses. The self-hosted parser now catches once and dispatches typed
  clauses in source order with `IS_TYPE`, preserving catch-all behavior and
  re-raising an exception unmatched by every filtered clause.

- Self-hosting, Phase 3 sub-phase 5 (seventy-second slice): unmatched rescue
  propagation. A value rejected by every inner filtered clause now re-raises
  into an outer matching rescue, with native/self-hosted runtime parity.

- Self-hosting, Phase 3 sub-phase 5 (seventy-third slice): catch-all ordering
  diagnostics. Any rescue clause following a catch-all is unreachable and is
  now rejected identically by both compilers.

- Self-hosting, Phase 3 sub-phase 5 (seventy-fourth slice): cross-clause
  duplicate filters. Rescue dispatch now tracks previously handled type IDs
  and rejects exact repeats in later clauses with native diagnostic parity.

- Self-hosting, Phase 3 sub-phase 5 (seventy-fifth slice): later-clause retry.
  A `retry` executed from a later matching rescue restarts the protected body
  and preserves the eventual expression result in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (seventy-sixth slice): multiple-rescue
  ensure integration. A shared `ensure` executes exactly once after a later
  rescue clause handles the exception, with matching result behavior.

- Self-hosting, Phase 3 sub-phase 5 (seventy-seventh slice): multiple-rescue
  else integration. The `else` body runs only after normal protected-body
  completion and supplies the expression result without entering handlers.

- Self-hosting, Phase 3 sub-phase 5 (seventy-eighth slice): later-clause bare
  re-raise. A bare `raise` in a later matching clause preserves the current
  exception and propagates it to an outer handler in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (seventy-ninth slice): ordered union-filter
  dispatch. Multi-type filters preserve source-order clause selection while
  matching every member of each clause in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (eightieth slice): nominal subclass rescue
  matching. A later clause filtered by a superclass catches a raised subclass
  instance through the VM's nominal `IS_TYPE` semantics in both compilers.

- Self-hosting, Phase 3 sub-phase 5 (eighty-first slice): superclass-shadowed
  rescue diagnostics. Class metadata now retains superclass indexes so a later
  subclass filter already covered by an earlier superclass is rejected.

- Self-hosting, Phase 3 sub-phase 5 (eighty-second slice): exact multi-rescue
  result facts. Matching exact results across the protected body and every
  rescue clause retain their fact for downstream typed dispatch.

- Self-hosting, Phase 3 sub-phase 5 (eighty-third slice): final catch-all
  dispatch. Exceptions rejected by earlier filtered clauses now reach a final
  catch-all clause in source order with native runtime parity.

- Self-hosting, Phase 3 sub-phase 5 (eighty-fourth slice): declared
  multi-rescue result joins. Matching declared unions across the protected
  body and every handler remain available for downstream type narrowing.

- Self-hosting, Phase 3 sub-phase 5 (eighty-fifth slice): later-clause binding
  contracts. Each rescue binding receives its clause's filtered type, allowing
  a later handler to pass the exception into a matching typed call.

- Self-hosting, Phase 3 follow-up (eighty-sixth slice): `next` loop control.
  Loop frames now carry condition-reentry and body-reentry targets; `next`
  jumps to condition reevaluation for `while`/`until` and body start for loop.

- Self-hosting, Phase 3 follow-up (eighty-seventh slice): `redo` loop control.
  `redo` jumps directly to the active body target without reevaluating the
  condition, matching native behavior when the condition has become false.

- Self-hosting, Phase 3 follow-up (eighty-eighth slice): unconditional-loop
  `next`. A skipped iteration re-enters the body target while a later valued
  `break` still supplies the loop expression result.

- Self-hosting, Phase 3 follow-up (eighty-ninth slice): nested loop-control
  isolation. An inner `next` targets only the innermost loop frame and leaves
  the outer loop's progress and output intact.

- Self-hosting, Phase 3 follow-up (ninetieth slice): `next` context diagnostics.
  A top-level `next` is rejected identically by the native and self-hosted
  parsers when no active loop frame supplies a continuation target.

- Self-hosting, Phase 3 follow-up (ninety-first slice): `redo` context
  diagnostics. A top-level `redo` is rejected identically when no active loop
  frame supplies a body-reentry target.

- Self-hosting, Phase 3 follow-up (ninety-second slice): `next` value
  diagnostics. Unlike value-bearing `break`, `next` rejects a following value
  identically in the native and self-hosted parsers.

- Self-hosting, Phase 3 follow-up (ninety-third slice): `redo` value
  diagnostics. `redo` likewise rejects a following value through the shared
  loop-control contract in both compilers.

- Self-hosting, Phase 3 follow-up (ninety-fourth slice): `next` in `until`.
  Skipped iterations jump back through the inverted condition path and retain
  native output ordering.

- Self-hosting, Phase 3 follow-up (ninety-fifth slice): `redo` in unconditional
  loops. Body re-entry repeats the iteration without resetting the loop result,
  allowing a later valued `break` to complete the expression normally.

- Self-hosting, Phase 3 follow-up (ninety-sixth slice): nested `redo`
  isolation. An inner `redo` targets only the innermost body and preserves the
  outer loop's iteration progress and output order.

- Self-hosting, Phase 3 follow-up (ninety-seventh slice): `redo` in `until`.
  Body re-entry bypasses the inverted condition even after it becomes true,
  then ordinary back-edge evaluation exits the loop.

- Self-hosting, Phase 3 follow-up (ninety-eighth slice): function-local
  `next`. Loop continuation targets compile correctly inside a named function's
  independent bytecode and register frame.

- Self-hosting, Phase 3 follow-up (ninety-ninth slice): method-local `redo`.
  Body-reentry targets compile correctly inside an instance method and preserve
  a later valued loop result.

- Self-hosting, Phase 3 follow-up (one-hundredth slice): method-local `next`.
  Continue targets compile correctly inside an instance method, skipping the
  rest of the current iteration while preserving later method-local work.

- Self-hosting, Phase 3 follow-up (one-hundred-first slice): postfix `if`.
  Statement-level modifier lookahead and bytecode re-entry now skip or execute
  an ordinary expression without evaluating its body eagerly.

- Self-hosting, Phase 3 follow-up (one-hundred-second slice): postfix `unless`.
  Inverted modifier branches execute only for false conditions and preserve
  the same lazy statement-body behavior as postfix `if`.

- Self-hosting, Phase 3 follow-up (one-hundred-third slice): conditioned
  assignment. Postfix branches apply local mutation only on their taken path,
  leaving the prior value unchanged when the condition skips the statement.

- Self-hosting, Phase 3 follow-up (one-hundred-fourth slice): postfix `next`.
  Modifier keywords terminate loop-control value scanning, and a taken branch
  continues at the active loop's condition target without running later work.

- Self-hosting, Phase 3 follow-up (one-hundred-fifth slice): postfix `redo`.
  A taken modifier re-enters the active loop body directly, while later false
  conditions fall through and allow ordinary loop progress.

- Self-hosting, Phase 3 follow-up (one-hundred-sixth slice): postfix `break`.
  A conditioned valued exit updates the loop result only on the taken branch
  and otherwise permits later iterations to reach their own exit.

- Self-hosting, Phase 3 follow-up (one-hundred-seventh slice): postfix bare
  `return`. Modifier boundaries produce a nil early return on the taken path
  while a false condition falls through to the rest of the function body.

- Self-hosting, Phase 3 follow-up (one-hundred-eighth slice): postfix bare
  `raise`. A taken rescue-local modifier re-raises the current exception value,
  while a false condition continues through the handler normally.

- Self-hosting, Phase 3 follow-up (one-hundred-ninth slice): postfix `retry`.
  A rescue-local modifier jumps to the protected body only while its condition
  is true, then falls through with the final attempt state intact.

- Self-hosting, Phase 3 follow-up (one-hundred-tenth slice): nested modifier
  targeting. Rescue-local `retry` re-enters only its protected body, and a
  later `next` still continues the independently enclosing loop.

- Self-hosting, Phase 3 follow-up (one-hundred-eleventh slice): endless
  functions. An equals-delimited expression body compiles directly to the
  function result, including ordinary return-contract enforcement.

- Self-hosting, Phase 3 follow-up (one-hundred-twelfth slice): endless methods.
  Equals-delimited instance-method bodies compile in the receiver frame and
  retain parameter and return-contract enforcement.

- Self-hosting, Phase 3 follow-up (one-hundred-thirteenth slice): conditioned
  endless functions. Postfix `if` and `unless` lazily select the expression
  body, returning nil when its modifier condition skips execution.

- Self-hosting, Phase 3 follow-up (one-hundred-fourteenth slice): conditioned
  endless methods. Modifier branches execute receiver-local expression bodies
  lazily and return nil from skipped method invocations.

- Self-hosting, Phase 3 follow-up (one-hundred-fifteenth slice): declaration
  modifier diagnostics. Postfix `if` and `unless` following a completed
  declaration now receive the native compiler's explicit rejection.

- Self-hosting, Phase 3 follow-up (one-hundred-sixteenth slice): class
  declaration modifier diagnostics. Completed class bodies retain the same
  explicit postfix rejection as function declarations.

- Self-hosting, Phase 3 follow-up (one-hundred-seventeenth slice): interface
  declaration modifier diagnostics. Completed structural-interface bodies also
  reject trailing postfix conditions with the declaration-specific message.

- Self-hosting, Phase 3 follow-up (one-hundred-eighteenth slice): endless
  nested closures. Equals-delimited nested functions retain captured outer
  locals and restore the enclosing function's compiler state after emission.

- Self-hosting, Phase 3 follow-up (one-hundred-nineteenth slice): typed
  conditioned endless methods. Taken values and skipped nil results both pass
  through the declared union return contract.

- Self-hosting, Phase 3 follow-up (one-hundred-twentieth slice): bracket-aware
  modifier lookahead. Conditional expressions nested inside array literals are
  ignored, while a true trailing condition on a single-line literal remains
  discoverable.

- Self-hosting, Phase 3 follow-up (one-hundred-twenty-first slice):
  parenthesis-aware modifier lookahead. Nested conditional expressions remain
  inside their grouping, while trailing conditions on single-line groups work.

- Self-hosting, Phase 3 follow-up (one-hundred-twenty-second slice): brace-aware
  modifier lookahead. Conditional hash values remain nested, while a postfix
  condition after a complete single-line hash is still recognized.

- Self-hosting, Phase 3 follow-up (one-hundred-twenty-third slice): assignment
  RHS lookahead. An `if` immediately after `=` begins the assigned expression,
  while a later top-level `if` continues to act as a statement modifier.

- Self-hosting, Phase 3 follow-up (one-hundred-twenty-fourth slice): conditioned
  endless nested closures. Taken bodies read captured outer locals, and skipped
  invocations return nil without disturbing later closure calls.

- Self-hosting, Phase 3 follow-up (one-hundred-twenty-fifth slice): typed
  conditioned endless functions. Both concrete and skipped nil results flow
  through an explicit union return contract.

- Self-hosting, Phase 3 follow-up (one-hundred-twenty-sixth slice): module
  builder bridge. `ProgramBuilder#declare_module` now creates validated module
  descriptors for the self-hosted parser's upcoming namespace lowering.

- Self-hosting, Phase 3 follow-up (one-hundred-twenty-seventh slice): namespace
  constant builder bridge. Qualified names now reserve validated constant slots
  and expose their bytecode operand indexes to the self-hosted parser.

- Self-hosting, Phase 3 follow-up (one-hundred-twenty-eighth slice): empty
  module declarations. The self-hosted parser validates top-level module names,
  registers descriptors through ProgramBuilder, and restores namespace state.

- Self-hosting, Phase 3 follow-up (one-hundred-twenty-ninth slice): module
  constants. Uppercase assignments reserve qualified namespace slots and emit
  write-once `SET_NAMESPACE_CONSTANT` bytecode.

- Self-hosting, Phase 3 follow-up (one-hundred-thirtieth slice): qualified
  namespace constant reads. `Module::NAME` resolves through parser module
  metadata and emits `GET_NAMESPACE_CONSTANT` with the reserved slot index.

- Self-hosting, Phase 3 follow-up (one-hundred-thirty-first slice): namespace
  constant modifier diagnostics. A trailing postfix condition now receives the
  native compiler's module-body rejection message.

- Self-hosting, Phase 3 follow-up (one-hundred-thirty-second slice): lexical
  module constant reads. Later definitions can reference earlier constants in
  the active module and emit the same namespace-slot lookup as qualified reads.

- Self-hosting, Phase 3 follow-up (one-hundred-thirty-third slice): module
  constant diagnostics. Lowercase definitions and duplicate write-once names
  are rejected in the parser with native-compatible messages.

- Self-hosting, Phase 3 follow-up (one-hundred-thirty-fourth slice): module
  method builder bridge. Validated function descriptors can now be registered
  in a module's includable instance-method table.

- Self-hosting, Phase 3 follow-up (one-hundred-thirty-fifth slice): module
  instance methods. Module `def` bodies reuse the method compiler and register
  their descriptors against the active module with duplicate tracking.

- Self-hosting, Phase 3 follow-up (one-hundred-thirty-sixth slice): module
  inclusion builder bridge. `ProgramBuilder#include_module` copies module fields
  and includable methods into a target class and recomputes its shape metadata.

- Self-hosting, Phase 3 follow-up (one-hundred-thirty-seventh slice): class
  inclusion syntax. Class bodies resolve `include Module` against declared
  modules and apply the descriptor-copy bridge before later method definitions.

- Self-hosting, Phase 3 follow-up (one-hundred-thirty-eighth slice): included
  method dispatch. A module-owned typed method copied into a class resolves and
  executes through ordinary instance invocation.

- Self-hosting, Phase 3 follow-up (one-hundred-thirty-ninth slice): module
  method receivers. `self` resolves to register zero in module-owned method
  bodies and preserves the including instance's identity at invocation time.

- Self-hosting, Phase 3 follow-up (one-hundred-fortieth slice): module method
  lexical constants. Included methods retain namespace-slot reads compiled in
  their declaring module and return those values through instance dispatch.

- Self-hosting, Phase 3 follow-up (one-hundred-forty-first slice): module field
  builder bridge. `ProgramBuilder#declare_module_field` reserves stable symbolic
  field slots on module descriptors for later inclusion into class shapes.

- Self-hosting, Phase 3 follow-up (one-hundred-forty-second slice): module
  instance variables. Module methods emit symbolic ivar reads and writes while
  declaring fields for materialization in each including class shape.

- Self-hosting, Phase 3 follow-up (one-hundred-forty-third slice): included
  module state. Symbolic fields persist on their receiver and remain isolated
  across independently constructed instances of the including class.

- Self-hosting, Phase 3 follow-up (one-hundred-forty-fourth slice): transitive
  inclusion builder bridge. One module can copy another module's fields and
  includable methods through an index-safe dedicated ProgramBuilder operation.

- Self-hosting, Phase 3 follow-up (one-hundred-forty-fifth slice): transitive
  module inclusion. Module bodies resolve `include`, copy symbolic state and
  methods, and preserve both through a later class inclusion and invocation.

- Self-hosting, Phase 3 follow-up (one-hundred-forty-sixth slice): module
  self-inclusion diagnostics. The active module rejects including its own
  descriptor with the native compiler's explicit cycle message.

- Self-hosting, Phase 3 follow-up (one-hundred-forty-seventh slice): undefined
  transitive includes. Module bodies reject unresolved include names before any
  descriptor mutation, matching the native compiler's diagnostic.

- Self-hosting, Phase 3 follow-up (one-hundred-forty-eighth slice): direct
  module overrides. Locally declared methods may replace imported descriptors,
  and later class inclusion dispatches to the direct implementation.

- Self-hosting, Phase 3 follow-up (one-hundred-forty-ninth slice): multiple
  module precedence. When imported modules define the same method, the later
  include wins through reverse descriptor lookup.

- Self-hosting, Phase 3 follow-up (one-hundred-fiftieth slice): transitive
  override preservation. A direct method replacing an imported descriptor keeps
  precedence after another module layer and final class inclusion.

- Self-hosting, Phase 3 follow-up (one-hundred-fifty-first slice): module
  visibility builder bridge. Named direct module methods can be switched between
  private and public descriptors through a validated ProgramBuilder mutation.

- Self-hosting, Phase 3 follow-up (one-hundred-fifty-second slice): standalone
  module visibility modes. `private` and `public` switch the descriptor flag
  applied to subsequently compiled module methods.

- Self-hosting, Phase 3 follow-up (one-hundred-fifty-third slice): included
  private enforcement. A private module method retains its visibility after
  descriptor copying and external invocation raises the native `TypeError`.

- Self-hosting, Phase 3 follow-up (one-hundred-fifty-fourth slice): targeted
  module visibility. Named `private`/`public` directives mutate existing direct
  method descriptors, including targeted promotion back to public visibility.

- Self-hosting, Phase 3 follow-up (one-hundred-fifty-fifth slice): transitive
  private visibility. Private method descriptors retain their visibility while
  being copied through an intermediate module and then into a class.

- Self-hosting, Phase 3 follow-up (one-hundred-fifty-sixth slice): targeted
  visibility diagnostics. Named visibility directives reject methods that were
  not directly declared by the active module with the native diagnostic.

- Self-hosting, Phase 3 follow-up (one-hundred-fifty-seventh slice): multiple
  visibility targets. Comma-separated named directives update every direct
  descriptor and permit a later targeted promotion of one method.

- Self-hosting, Phase 3 follow-up (one-hundred-fifty-eighth slice):
  parenthesized module visibility. Named `private(...)` and `public(...)`
  directives accept the native compiler's call-like target-list form.

- Self-hosting, Phase 3 follow-up (one-hundred-fifty-ninth slice): empty
  visibility-list diagnostics. Parenthesized visibility directives require a
  method name and match the native compiler's rejection message.

- Self-hosting, Phase 3 follow-up (one-hundred-sixtieth slice): unterminated
  visibility-list diagnostics. Parenthesized target lists require their closing
  delimiter and match the native compiler's diagnostic.

- Self-hosting, Phase 3 follow-up (one-hundred-sixty-first slice): module
  singleton export bridge. `ProgramBuilder#export_module_method` privatizes a
  direct module method and installs its receiver-aware singleton descriptor.

- Self-hosting, Phase 3 follow-up (one-hundred-sixty-second slice): targeted
  module exports. `module_function name` exports a directly declared method and
  makes its subsequently included instance descriptor private.

- Self-hosting, Phase 3 follow-up (one-hundred-sixty-third slice): module
  export lists. `module_function` accepts comma-separated targets in both bare
  and parenthesized forms while privatizing every exported instance method.

- Self-hosting, Phase 3 follow-up (one-hundred-sixty-fourth slice): module
  export mode. A standalone `module_function` directive exports and privatizes
  each subsequently compiled direct module method.

- Self-hosting, Phase 3 follow-up (one-hundred-sixty-fifth slice): undefined
  module export diagnostics. Targeted `module_function` rejects names not
  directly declared by the active module with the native compiler's message.

- Self-hosting, Phase 3 follow-up (one-hundred-sixty-sixth slice): exported
  function metadata. Self-hosted module entries retain direct function indices
  and arities plus the ordered singleton-export descriptor set.

- Self-hosting, Phase 3 follow-up (one-hundred-sixty-seventh slice): qualified
  module calls. Exported descriptors lower `Module.function(...)` through a
  receiver-aware direct `CALL`, matching native `module_function` dispatch.

- Self-hosting, Phase 3 follow-up (one-hundred-sixty-eighth slice): module-call
  arity diagnostics. Qualified singleton calls validate their argument count
  against the exported method descriptor before emitting bytecode.

- Self-hosting, Phase 3 follow-up (one-hundred-sixty-ninth slice): undefined
  module-call diagnostics. Qualified calls reject names absent from the active
  module's singleton export set with the native compiler's message.

- Self-hosting, Phase 3 follow-up (one-hundred-seventieth slice): missing
  module-call names. A module qualifier followed by `.` requires an identifier
  before call arguments and reports the native undefined-singleton diagnostic.

- Self-hosting, Phase 3 follow-up (one-hundred-seventy-first slice): stateful
  targeted exports. Module method metadata records instance-state access and
  targeted `module_function` rejects such methods with the native diagnostic.

- Self-hosting, Phase 3 follow-up (one-hundred-seventy-second slice): stateful
  export-mode diagnostics. Methods compiled after standalone `module_function`
  reject instance-state access with the mode-specific native message.

- Self-hosting, Phase 3 follow-up (one-hundred-seventy-third slice): duplicate
  module exports. Re-exporting an existing singleton descriptor is rejected by
  the validated builder bridge with the native duplicate-export diagnostic.

- Self-hosting, Phase 3 follow-up (one-hundred-seventy-fourth slice): qualified
  module-call delimiters. Resolved singleton function names require an opening
  parenthesis and match the native missing-call-delimiter diagnostic.

- Self-hosting, Phase 3 follow-up (one-hundred-seventy-fifth slice):
  unterminated module export lists. Parenthesized `module_function` target lists
  require a closing delimiter and match the native compiler's diagnostic.

- Self-hosting, Phase 3 follow-up (one-hundred-seventy-sixth slice): writer
  method definitions. The self-hosted method parser preserves a trailing `=`
  as part of class and module method names before parsing parameters.

- Self-hosting, Phase 3 follow-up (one-hundred-seventy-seventh slice): writer
  module exports. Targeted `module_function value=` resolves and exports the
  complete suffixed direct-method descriptor.

- Self-hosting, Phase 3 follow-up (one-hundred-seventy-eighth slice): qualified
  writer calls. Module singleton lookup consumes the trailing `=` as part of
  the exported name and lowers its receiver-aware call normally.

- Self-hosting, Phase 3 follow-up (one-hundred-seventy-ninth slice): writer-call
  arity diagnostics. Qualified module writers validate their single argument
  against the exported descriptor before emitting a call.

- Self-hosting, Phase 3 follow-up (one-hundred-eightieth slice): duplicate
  writer exports. Re-exporting a suffixed singleton descriptor is rejected with
  the same native duplicate-module-function diagnostic as ordinary methods.

- Self-hosting, Phase 3 follow-up (one-hundred-eighty-first slice): nested
  module declarations. Module bodies may declare a lexical child module whose
  stored builder name is qualified with its parent namespace.

- Self-hosting, Phase 3 follow-up (one-hundred-eighty-second slice): nested
  module singleton calls. A two-segment qualified module name resolves its
  exported descriptor set before lowering the singleton call.

- Self-hosting, Phase 3 follow-up (one-hundred-eighty-third slice): qualified
  module includes. Class and module include directives consume a two-segment
  module name and resolve its fully qualified builder descriptor.

- Self-hosting, Phase 3 follow-up (one-hundred-eighty-fourth slice): nested
  module constants. Three-segment qualified reads resolve constants stored on a
  lexical child module through its namespace descriptor.

- Self-hosting, Phase 3 follow-up (one-hundred-eighty-fifth slice): undefined
  nested namespace diagnostics. Qualified paths whose child segment resolves
  to neither a module nor a constant report the native namespaced-name error.

- Self-hosting, Phase 3 follow-up (one-hundred-eighty-sixth slice): active
  module entries. Parser state now saves and restores the module table index
  independently from builder and qualified-name identities across nesting.

- Self-hosting, Phase 3 follow-up (one-hundred-eighty-seventh slice): restored
  outer module declarations. Methods declared after a nested module attach to
  the restored parent descriptor and remain exportable as parent singletons.

- Self-hosting, Phase 3 follow-up (one-hundred-eighty-eighth slice): lexical
  child includes. Nested modules retain their parent namespace and resolve an
  unqualified sibling include against that lexical scope.

- Self-hosting, Phase 3 follow-up (one-hundred-eighty-ninth slice): nested
  class declarations. Module bodies may compile child classes whose builder and
  parser identities carry the enclosing module's qualified namespace.

- Self-hosting, Phase 3 follow-up (one-hundred-ninetieth slice): qualified
  nested construction. Two-segment class names resolve through the class table
  and lower `Namespace::Class.new(...)` through the existing constructor path.

- Self-hosting, Phase 3 follow-up (one-hundred-ninety-first slice): lexical
  parent constants. Nested module methods fall back from their local constant
  table to the enclosing module's constants for unqualified reads.

- Self-hosting, Phase 3 follow-up (one-hundred-ninety-second slice): lexical
  sibling classes. Code compiled within a module resolves an unqualified class
  name against the active module namespace before constructor lowering.

- Self-hosting, Phase 3 follow-up (one-hundred-ninety-third slice): lexical
  superclasses. A nested class resolves an unqualified superclass name against
  its active module namespace before inheritance setup.

- Self-hosting, Phase 3 follow-up (one-hundred-ninety-fourth slice): nested
  interfaces. Module bodies may declare interfaces whose builder and parser
  type identities carry the enclosing module namespace.

- Self-hosting, Phase 3 follow-up (one-hundred-ninety-fifth slice): deeper
  module paths. Three module segments resolve their fully qualified singleton
  export descriptor before call lowering.

- Self-hosting, Phase 3 follow-up (one-hundred-ninety-sixth slice): qualified
  interface annotations. Type parsing preserves namespace segments so nested
  interface identities resolve in parameter and return contracts.

- Self-hosting, Phase 3 follow-up (one-hundred-ninety-seventh slice): nested
  interface qualification diagnostics. Unqualified nested interface names are
  rejected like the native compiler, requiring the explicit qualified form.

- Self-hosting, Phase 3 follow-up (one-hundred-ninety-eighth slice): deep
  module constants. Four-segment qualified reads select the deepest module's
  constant table before namespace-constant lowering.

- Self-hosting, Phase 3 follow-up (one-hundred-ninety-ninth slice): deep nested
  construction. Three namespace segments resolve a deeply nested class before
  lowering its constructor call.

- Self-hosting, Phase 3 follow-up (two-hundredth slice): qualified type path
  diagnostics. A namespace separator in an annotation requires a following
  identifier and matches the native qualified-type diagnostic.

- Self-hosting, Phase 3 follow-up (two-hundred-first slice): qualified include
  parsing. Class and module include paths share a compact, register-safe helper
  that consumes arbitrary namespace segments.

- Self-hosting, Phase 3 follow-up (two-hundred-second slice): deep qualified
  includes. Classes resolve and copy descriptors from module paths containing
  three namespace segments.

- Self-hosting, Phase 3 follow-up (two-hundred-third slice): module cross-kind
  collisions. Module declarations reject qualified names already occupied by a
  class or interface using the native duplicate-module diagnostic.

- Self-hosting, Phase 3 follow-up (two-hundred-fourth slice): class cross-kind
  collisions. Class declarations reject names already occupied by modules with
  the native duplicate-type diagnostic.

- Self-hosting, Phase 3 follow-up (two-hundred-fifth slice): interface
  cross-kind collisions. Interface declarations reject names already occupied
  by modules with the native duplicate-type diagnostic.

- Self-hosting, Phase 3 follow-up (two-hundred-sixth slice): lexical base
  interfaces. Nested interface declarations resolve unqualified base names
  against their active module namespace before inheriting method contracts.

- Self-hosting, Phase 3 follow-up (two-hundred-seventh slice): nested
  class/interface collisions. Qualified class declarations reject an interface
  already occupying the same lexical namespace name.

- Self-hosting, Phase 3 follow-up (two-hundred-eighth slice): nested
  module/interface collisions. Qualified module declarations reject an
  interface already occupying the same lexical namespace name.

- Self-hosting, Phase 3 follow-up (two-hundred-ninth slice): deep lexical
  constants. Unqualified constant reads walk every enclosing module namespace
  until they find a matching declaration.

- Self-hosting, Phase 3 follow-up (two-hundred-tenth slice): nested
  interface/class collisions. Qualified interface declarations reject a class
  already occupying the same lexical namespace name.

- Self-hosting, Phase 3 follow-up (two-hundred-eleventh slice): source-expansion
  bridge. `ProgramBuilder#expand_source` exposes the native loader's canonical
  require expansion as a GC-managed String for self-hosted compilation.

- Self-hosting, Phase 3 follow-up (two-hundred-twelfth slice): direct require
  expansion. The builder bridge resolves and inlines an existing required file
  through the native loader's canonical path rules.

- Self-hosting, Phase 3 follow-up (two-hundred-thirteenth slice): duplicate
  require suppression. A single expansion tracks canonical loaded paths and
  avoids inlining the same required source twice.

- Self-hosting, Phase 3 follow-up (two-hundred-fourteenth slice): circular
  require propagation. Loader cycle failures surface through the bridge as an
  `IOError` that self-hosted callers can diagnose or rescue.

- Self-hosting, Phase 3 follow-up (two-hundred-fifteenth slice): file-based
  self-hosted loading. The parser runner expands a source file's complete
  require graph into its shared builder before compilation and execution.

- Self-hosting, Phase 3 follow-up (two-hundred-sixteenth slice): error-runner
  loading. Self-hosted negative differential checks expand the source file's
  require graph through the same native loader bridge before parsing.

- Self-hosting, Phase 3 follow-up (two-hundred-seventeenth slice): nested
  require execution. File-based self-hosted runs recursively inline multiple
  require levels and compile their functions into one builder program.

- Self-hosting, Phase 3 follow-up (two-hundred-eighteenth slice): duplicate
  require execution. Requiring the same canonical file twice still declares
  and executes its contents only once in the self-hosted pipeline.

- Self-hosting, Phase 3 follow-up (two-hundred-nineteenth slice): circular
  require diagnostics. File-based self-hosted loading preserves the native
  loader's cycle message and including-file location.

- Self-hosting, Phase 3 follow-up (two-hundred-twentieth slice): missing
  require diagnostics. Self-hosted error checks preserve the loader's resolved
  missing-file message before parser construction begins.

- Self-hosting, Phase 3 follow-up (two-hundred-twenty-first slice): stable
  package fixture. A repository-local package with a valid manifest provides a
  deterministic target for loader-bridge and self-hosted require coverage.

- Self-hosting, Phase 3 follow-up (two-hundred-twenty-second slice): package
  execution. The file-based self-hosted runner resolves a bare package name,
  validates its manifest, and compiles the package function into the program.

- Self-hosting, Phase 3 follow-up (two-hundred-twenty-third slice): package
  manifest diagnostics. A mismatched declared package name propagates through
  the self-hosted error runner with the native loader message.

- Self-hosting, Phase 3 follow-up (two-hundred-twenty-fourth slice): package
  manifest shape diagnostics. A manifest returning a non-Hash value is rejected
  through the self-hosted loader bridge with the native message.

- Self-hosting, Phase 3 follow-up (two-hundred-twenty-fifth slice): package
  manifest version diagnostics. A present non-String version field is rejected
  through the self-hosted loader bridge with the native message.

- Self-hosting, Phase 3 follow-up (two-hundred-twenty-sixth slice): missing
  package names. Manifests without a `name` key are rejected through the
  self-hosted loader bridge with the native contract diagnostic.

- Self-hosting, Phase 3 follow-up (two-hundred-twenty-seventh slice): typed
  package names. Manifests whose `name` value is not a String are rejected with
  the same native contract diagnostic as a missing name.

- Self-hosting, Phase 3 follow-up (two-hundred-twenty-eighth slice): package
  local requires. Required package source resolves a nested relative require
  from the package directory and compiles both files into one program.

- Self-hosting, Phase 3 follow-up (two-hundred-twenty-ninth slice): duplicate
  package suppression. Requiring the same bare package twice validates and
  compiles its canonical source only once.

- Self-hosting, Phase 3 follow-up (two-hundred-thirtieth slice): manifest
  compilation diagnostics. Syntax failures in package manifests propagate
  through the loader bridge with their native validation context.

- Self-hosting, Phase 3 follow-up (two-hundred-thirty-first slice): manifest
  runtime diagnostics. Exceptions raised while evaluating package metadata
  propagate through the loader bridge with native manifest context.

- Self-hosting, Phase 3 follow-up (two-hundred-thirty-second slice): require
  precedence. A relative file matching a bare require name wins over an
  installed package in the self-hosted loading pipeline.

- Self-hosting, Phase 3 follow-up (two-hundred-thirty-third slice): optional
  package versions. A package manifest containing only its required name is
  accepted and its source executes through the self-hosted loading pipeline.

- Self-hosting, Phase 3 follow-up (two-hundred-thirty-fourth slice): missing
  package-local dependencies. A missing relative dependency required by
  package source preserves the native loader diagnostic and package context.

- Self-hosting, Phase 3 follow-up (two-hundred-thirty-fifth slice): circular
  package-local requires. A package helper requiring its active package source
  is rejected with the native circular-require diagnostic.

- Self-hosting, Phase 3 follow-up (two-hundred-thirty-sixth slice): require
  depth parity. A generated 129-file dependency chain confirms that the
  self-hosted loader bridge preserves the native nesting-limit diagnostic.

- Self-hosting, Phase 3 follow-up (two-hundred-thirty-seventh slice): loaded
  file parity. A generated 128-dependency fan-out confirms that the self-hosted
  loader bridge preserves the native loaded-file-limit diagnostic.

- Self-hosting, Phase 3 follow-up (two-hundred-thirty-eighth slice): imported
  compile locations. Required-file syntax failures retain the dependency path
  and original line through the self-hosted loader and parser pipeline.

- Self-hosting, Phase 3 follow-up (two-hundred-thirty-ninth slice): imported
  runtime locations. ProgramBuilder bytecode records self-host lexer positions,
  preserving required-function and root-caller frames with native line/column
  parity.

- Self-hosting, Phase 3 follow-up (two-hundred-fortieth slice): manifest read
  diagnostics. A package whose manifest path is not a readable regular file
  preserves the native manifest-specific I/O context through the self-hosted
  loader bridge.

- Self-hosting, Phase 3 follow-up (two-hundred-forty-first slice): imported EOF
  locations. An unterminated required file retains its dependency path at the
  expanded-source segment boundary in self-hosted parser diagnostics.

- Self-hosting, Phase 3 follow-up (two-hundred-forty-second slice): multiline
  imported EOF locations. EOF diagnostics advance from a dependency segment's
  original starting line and report the same final line as the native parser.

- Self-hosting, Phase 3 follow-up (two-hundred-forty-third slice): CRLF import
  diagnostics. Dynamically generated CRLF root and dependency files preserve
  the imported compile-error path and line through both parser pipelines.

- Self-hosting, Phase 3 follow-up (two-hundred-forty-fourth slice): CRLF runtime
  locations. Required-function and root-caller frames retain native path and
  line/column parity when both source files use CRLF line endings.

- Self-hosting, Phase 3 follow-up (two-hundred-forty-fifth slice): source-map
  GC retention. Required-function and root-caller locations remain intact when
  every self-host allocation triggers a garbage collection.

- Self-hosting, Phase 3 follow-up (two-hundred-forty-sixth slice): nested import
  diagnostics. A syntax error reached through an intermediate dependency maps
  to the leaf file and original line in both parser pipelines.

- Self-hosting, Phase 3 follow-up (two-hundred-forty-seventh slice): nested
  runtime locations. Leaf, intermediate, and root frames survive a two-level
  required-file chain with native/self-hosted source-location parity.

- Self-hosting, Phase 3 follow-up (two-hundred-forty-eighth slice): duplicate
  require mapping. Canonically identical dependency spellings emit one source
  segment and preserve one stable imported runtime frame.

- Self-hosting, Phase 3 follow-up (two-hundred-forty-ninth slice): package
  compile locations. Syntax failures in validated package source map to the
  canonical package file and original line through both parser pipelines.

- Self-hosting, Phase 3 follow-up (two-hundred-fiftieth slice): package runtime
  locations. Runtime failures retain the package function's line/column and
  the requiring root frame through the self-hosted execution path.

- Self-hosting, Phase 3 follow-up (two-hundred-fifty-first slice): package-local
  compile locations. Syntax failures in a package's relative helper map to the
  helper's canonical path and original line through both parser pipelines.

- Self-hosting, Phase 3 follow-up (two-hundred-fifty-second slice): package-local
  runtime locations. Helper, package entry, and requiring-root frames survive
  nested package loading with native/self-hosted location parity.

- Self-hosting, Phase 3 follow-up (two-hundred-fifty-third slice): duplicate
  package mapping. Requiring one package twice emits one canonical source
  segment and retains a single stable runtime frame.

- Self-hosting, Phase 3 follow-up (two-hundred-fifty-fourth slice): package-map
  GC retention. Package helper, entry, and root locations remain intact when
  every allocation in the self-hosted execution path triggers collection.

- Self-hosting, Phase 3 follow-up (two-hundred-fifty-fifth slice): source-map
  replacement. Re-expanding through one ProgramBuilder releases the previous
  bundle and resolves subsequent locations only against the replacement map.

- Self-hosting, Phase 3 follow-up (two-hundred-fifty-sixth slice): string
  interpolation. The self-hosted parser now saves and restores lexer state,
  parses embedded expressions, stringifies their values, and joins the pieces.

- Self-hosting, Phase 3 follow-up (two-hundred-fifty-seventh slice): interpolated
  scalar conversion. Integer, Float, Bool, Nil, and String values stringify
  with native runtime parity inside one interpolated string.

- Self-hosting, Phase 3 follow-up (two-hundred-fifty-eighth slice): repeated
  interpolation. Adjacent substitutions and surrounding literal pieces join
  in source order without losing empty boundary segments.

- Self-hosting, Phase 3 follow-up (two-hundred-fifty-ninth slice): escaped
  interpolation markers. An escaped hash remains literal text and never opens
  an embedded lexer, matching native string decoding.

- Self-hosting, Phase 3 follow-up (two-hundred-sixtieth slice): structured
  interpolation expressions. Embedded grouping, array construction, indexing,
  and arithmetic compile through the ordinary expression pipeline.

- Self-hosting, Phase 3 follow-up (two-hundred-sixty-first slice): interpolated
  calls. Embedded expressions resolve locals and invoke user functions before
  converting the returned value to String.

- Self-hosting, Phase 3 follow-up (two-hundred-sixty-second slice): unterminated
  interpolation diagnostics. Missing closing braces are rejected by both
  lexer/parser pipelines instead of consuming the outer string state.

- Self-hosting, Phase 3 follow-up (two-hundred-sixty-third slice): imported
  interpolation. Interpolated function bodies loaded from a dependency compile
  and execute through the self-hosted source-expansion pipeline.

- Self-hosting, Phase 3 follow-up (two-hundred-sixty-fourth slice): interpolated
  runtime locations. Failures inside an imported embedded expression retain
  the dependency function and requiring-root frames.

- Self-hosting, Phase 3 follow-up (two-hundred-sixty-fifth slice): interpolation
  GC retention. Embedded-expression bytecode and imported source locations
  remain valid when every self-host allocation triggers collection.

- Self-hosting, Phase 3 follow-up (two-hundred-sixty-sixth slice): nested
  interpolation frames. A failing embedded expression compiled two requires
  deep reports the same leaf, mid, and root frames in both compilers.

- Self-hosting, Phase 3 follow-up (two-hundred-sixty-seventh slice):
  duplicate-require interpolation. Requiring the same interpolation source
  under two spellings still emits a single dependency frame in both
  compilers.

- Self-hosting, Phase 3 follow-up (two-hundred-sixty-eighth slice): CRLF
  interpolation frames. An embedded expression failing inside a
  CRLF-terminated required file reports matching line and column locations
  in both compilers.

- Self-hosting, Phase 3 follow-up (two-hundred-sixty-ninth slice): package
  interpolation frames, and a real root-name bug this uncovered. Requiring a
  package whose combined path exceeded 64 bytes silently dropped the
  self-hosted root frame to `<main>`, because `ProgramBuilder#expand_source`
  copied it into `DiamondFunction.name`, a buffer shared with every ordinary
  function/class name (`DIAMOND_MAX_FUNCTION_NAME`). `DiamondProgram` now
  carries a dedicated `entry_path` field sized to `DIAMOND_MAX_SOURCE_PATH`,
  and `diamond_program_chunk` prefers it once set. A package fixture whose
  path exceeds the old limit brings the parser harness in line with native,
  which never had this bug (`run_source` overwrites `chunk.name` with its
  own unbounded pointer after compiling).

- Self-hosting, Phase 3 follow-up (two-hundred-seventieth slice):
  package-local interpolation frames. An embedded expression failing inside
  a package's own internally required helper reports matching helper,
  package-entry, and root frames in both compilers.

- Self-hosting, Phase 3 follow-up (two-hundred-seventy-first slice):
  duplicate-package interpolation. Requiring the same interpolation-bearing
  package twice still emits a single dependency frame in both compilers,
  completing the interpolation source-map matrix (basic, scalar, repeated,
  escaped, structured, calls, unterminated, imported, GC-stressed, nested,
  duplicate-require, CRLF, package, package-local, and duplicate-package).

- Self-hosting, Phase 3 follow-up (two-hundred-seventy-second slice):
  imported unterminated-interpolation diagnostics. A missing closing brace
  inside a required file's string now reports that file's own path, line,
  and column in both compilers, joining the general `require_*` error
  corpus rather than a bespoke `parser_diff.sh` block.

- Self-hosting, Phase 3 follow-up (two-hundred-seventy-third slice): CRLF
  unterminated-interpolation diagnostics. A missing closing brace inside a
  CRLF-terminated required file reports the same line and column in both
  compilers.

- Self-hosting, Phase 3 follow-up (two-hundred-seventy-fourth slice): nested
  unterminated-interpolation diagnostics. A missing closing brace two
  requires deep still reports the leaf file's own path, line, and column in
  both compilers.

- Self-hosting, Phase 3 follow-up (two-hundred-seventy-fifth slice): package
  unterminated-interpolation diagnostics. A missing closing brace inside a
  required package's entry file reports that package file's own path, line,
  and column in both compilers.

- Self-hosting, Phase 3 follow-up (two-hundred-seventy-sixth slice): `File.open`
  recognition. `selfhost/parser.di` now recognizes `File.open(path, mode)`
  as a dedicated construct (mirroring `compiler.c`'s `parse_file_open_call`)
  and lowers it to `FILE_OPEN`, closing a real gap: the self-hosted parser
  had never supported the one native-recognized construct its own source
  (`parse_and_run`'s `File.open(path, "r").read()`) actually uses. The
  dispatch condition (class/local/function shadowing plus the `.` lookahead)
  moved into its own `is_file_open_target` helper rather than inlining it
  into `parse_name`, which was already close enough to the 256-register
  ceiling (see Phase 2's own register-budget note) that a naive inline
  `&&` chain pushed it over. A `File.open(...).read()` round trip against
  a real fixture file joins the differential parser-case corpus.

  Manually probing with the self-hosted parser pointed at its own two
  source files (`selfhost/lexer.di` and `selfhost/parser.di`) confirms
  `File.open` support alone is not sufficient for genuine
  self-compilation: it now gets past reading its own source and fails
  further in, on `attr_reader` (a class-body construct, not a call-site
  one) in `lexer.di`'s `Token` class. Left for a future slice; not
  something this round's dispatch-based fix touches.

- Self-hosting, Phase 3 follow-up (two-hundred-seventy-seventh slice):
  reclaim wasted class/module-member registers in `compile_definition`
  (`src/compiler.c`). Root-caused the register ceiling the File.open slice
  ran into: every `def` permanently reserves one register in its
  *enclosing* scope's frame via `const uint8_t result =
  allocate_register(compiler);`, and Diamond's register allocator is
  monotonic and never recycles within a function body (the same
  constraint the roadmap's Phase 2 and sub-phase 3 entries already
  flagged, now hit a fourth time). For a method or module function,
  `compiler->function` is still the program's top-level entry function
  while its enclosing `class`/`module` body compiles, so that reservation
  comes out of the *entry* function's own 256-register budget, not a
  per-class one — and both call sites that reach this path
  (`compile_class`'s and `compile_module`'s own `DIAMOND_TOKEN_DEF`
  branches) immediately discard the return value with an explicit
  `(void)`. Confirmed empirically before touching anything: instrumenting
  `allocate_register` and `compile_definition` directly showed the entry
  function's register count climbing by exactly one for every method
  compiled inside `class Parser` (into the 220s-250s partway through its
  ~113 methods), and swapping one existing method for one new one (net
  method count unchanged) made a failing build compile clean again --
  ruling out per-method body size or file position, isolating it to a
  pure per-member-declaration count. Fixed by skipping the allocation
  when `at_top_level && (current_class>=0||current_module>=0)`, returning
  the placeholder `0` instead -- safe precisely because that value is
  never read on either call site; a genuine top-level `def` (whose value
  `compile_sequence` may thread through as the whole sequence's result)
  keeps allocating as before. Verified with `make test-all` (debug/
  release/sanitizer builds, every C-level test, `tests/lexer_diff.sh`'s
  820 cases, and the full parser differential harness) -- all pass
  unchanged. This is a shared native-compiler fix, not specific to
  self-hosting, but it directly unblocks the next slice below.

- Self-hosting, Phase 3 follow-up (two-hundred-seventy-eighth slice):
  `gets`/`chr`/`to_f`/`to_i`/`to_sym` recognition. `selfhost/parser.di`
  now recognizes all five as dedicated LEFT_PAREN-triggered constructs
  (mirroring `compiler.c`'s `parse_gets_call`/`parse_chr_call`/
  `parse_to_float_call`/`parse_to_int_call`/`parse_to_sym_call`),
  including the same function-name shadowing check native applies before
  treating the bare name as a builtin. The four scalar-conversion
  builtins share one `parse_scalar_conversion_call(opcode, type_fact)`
  helper rather than four near-identical bodies. A round trip through
  all five joins the differential parser-case corpus. Directly depended
  on the prior slice's register-budget fix -- this shape (one dispatch
  helper plus five small call-parsers) was the exact amount of new
  class-member surface that broke without it.

- Self-hosting, Phase 3 follow-up (two-hundred-seventy-ninth slice):
  `sqrt`/`sin`/`cos`/`tan`/`pow` recognition. `selfhost/parser.di` now
  recognizes all five `Math` builtins (mirroring `compiler.c`'s
  `parse_math_unary_call`/`parse_math_binary_call`, encoding the specific
  function as a `DiamondMathFunction` byte operand rather than a distinct
  opcode per function). Two new dispatch lines in `parse_name`'s
  LEFT_PAREN handling were, on their own, enough to overflow *that
  function's own* register budget -- a different failure mode than the
  entry-scope waste the prior two slices fixed, and the same one Phase
  2's lexer port first hit: one large function covering many branches,
  not a shared/cumulative cost. Fixed by extracting the whole LEFT_PAREN
  dispatch chain (closures, `puts`/`print`, the scalar builtins, now the
  Math builtins) into its own `parse_name_call` method, giving it a fresh
  256-register budget separate from the rest of `parse_name`. A round
  trip through all five joins the differential parser-case corpus.

- Self-hosting, Phase 3 follow-up (two-hundred-eightieth slice):
  `Fiber.new`/`Regexp.new`/`TCPSocket.connect`/`TCPServer.listen`
  recognition, and raising `DIAMOND_MAX_METHODS` 128 -> 256. Adding these
  four DOT-triggered constructs (mirroring `compiler.c`'s
  `parse_fiber_new_call`/`parse_regexp_new_call`/`parse_tcp_connect_call`/
  `parse_tcp_listen_call`) pushed `class Parser` to 131 methods, past
  `DIAMOND_MAX_METHODS`'s old 128-entry cap on `DiamondClass.methods[]`.
  Unlike `DIAMOND_MAX_FUNCTIONS`/`DIAMOND_MAX_CONSTANTS` (Phase 0's
  follow-up correction found those genuinely capped at 256 by one-byte
  `CALL`/`CONSTANT` operands), nothing indexes into a class's own
  `methods[]` by position -- methods resolve by name via linear scan at
  runtime, and `DiamondMethod.function_index` is a separate index into
  the already-256-capped global function table. So this is a pure
  capacity constant, not a hard ceiling, and raising it (matching Phase
  0's own precedent of raising it once already, 32 -> 128) is safe.
  `tests/run.sh`'s interface-composition overflow regression (two
  70-method interfaces, chosen specifically to exceed the old 128 cap)
  needed the same treatment Phase 0 gave it originally: raised to two
  130-method interfaces to keep exercising the real overflow path at the
  new limit. `Regexp.new`'s optional-options default (a compile-time `0`
  constant when the second argument is omitted) reuses the same
  `add_constant`/`CONSTANT`-emission path integer literals already use.
  `TCPSocket.connect`/`TCPServer.listen` differential-verified manually
  (a real loopback connection and refusal, and a real ephemeral-port
  listen/close) rather than through the exact-match parser-case corpus,
  since real socket I/O isn't suitable for that harness's byte-for-byte
  comparison; a `Fiber.new`/`Regexp.new`/`TCPServer.listen` round trip
  that *is* deterministic joins the differential corpus instead.
  `redefine_method` deliberately deferred: it requires class singleton
  methods beyond `.new`, which `parse_name`'s `class_entry != nil` DOT
  branch doesn't support at all yet (it unconditionally assumes `.new`) --
  a separate, larger gap than a single dispatch addition.

  Manually probing turned up an unrelated, pre-existing gap while testing
  `TCPSocket.connect` inside a `rescue error: IOError` clause: rescue-type
  and parameter/return-type annotations resolve unknown names through
  `resolve_type_name` -> `find_class`, but `find_class` only tracks
  classes the self-hosted parser itself compiled from `class ... end`
  syntax -- the native VM's built-in exception hierarchy
  (`Exception`/`RuntimeError`/`IOError`/etc.) is constructed directly in
  `diamond_program_init` (`src/compiler.c`) and never goes through any
  `.di` source at all, so the self-hosted parser has no way to see it.
  Confirmed this isn't specific to `IOError`: `rescue error: RuntimeError`
  fails identically. The existing rescue-filter differential corpus never
  caught this because every case filters on a locally-declared exception
  subclass, never a bare built-in name. Left for a future slice.

- Self-hosting, Phase 3 follow-up (two-hundred-eighty-first slice):
  `attr`/`attr_reader`/`attr_writer`/`attr_accessor` recognition. Rather
  than mirroring `compiler.c`'s `compile_attribute_named` (which
  hand-assembles bytecode by writing straight into `DiamondFunction`'s C
  struct fields), the self-hosted version synthesizes each generated
  reader/writer through the *same* `declare_function`/`declare_field`/
  `declare_method` bridge calls and register-scoping save/restore
  `compile_method_body` already uses for ordinary method bodies -- a
  four-or-five-instruction body (`GET_IVAR`/`SET_IVAR` then `RETURN`)
  instead of a parsed one. `attr_predicate` isn't ported: unused by
  either self-hosted source file, and native's own version is a plain
  reader with a `?`-suffixed name (not an actual Bool conversion), a
  third method-naming case for no behavioral difference over
  `attr_reader`. A four-name round trip (`attr`, `attr_reader` with two
  names, `attr_writer`, `attr_accessor`) joins the differential
  parser-case corpus, verifying every generated reader.

  Also surfaced, independent of the fix above: calling *any*
  `=`-suffixed method via `.name=(value)` isn't supported by
  `compile_invoke` at all -- it unconditionally fails after an
  identifier unless a bare `(` follows, with no case for `=` first.
  Confirmed this isn't attr-specific (a plain hand-written `def
  name=(value)` has the same problem) and confirmed native itself
  requires exactly that `.name=(value)` call syntax (bare `receiver.field
  = value` is rejected by both compilers identically) rather than some
  alternate assignment-target path selfhost could fall back to. This
  means `attr_writer`/`attr_accessor`'s generated writer, while
  correctly declared, has no way to be invoked yet through the
  self-hosted parser -- verified via the reader half and a declaration
  that doesn't error, not a full write/read round trip. Left for a
  future slice.

  Directly checked whether `attr_reader` was actually sufficient for
  the self-hosted parser to parse its own source end-to-end (the
  original motivation, per the File.open slice's own probe): pointing
  it at `selfhost/lexer.di` now gets past the `attr_reader` line in
  `Token` and fails later, at `self.make_token(:eof)` -- a *much*
  larger, previously-unknown gap: `parse_prefix` has no case for
  `:symbol`-kind tokens at all, so no target program using a bare
  symbol literal can compile through the self-hosted parser, a
  construct both self-hosted source files use pervasively (every
  `@current.kind() == :foo` comparison is itself one). Given how
  fundamental this is, it's the next slice, not folded into this one.

- Self-hosting, Phase 3 follow-up (two-hundred-eighty-second slice): symbol
  literal expressions. `parse_prefix` now recognizes `:symbol`-kind tokens
  (mirroring `compiler.c`'s `parse_symbol`), stripping the leading `:` and
  adding the rest as a name string via the same `add_string` bridge call
  `compile_ivar_read`/`compile_ivar_write` already use for field names,
  then emitting `SYMBOL`. A literal/comparison round trip (including a
  `?`-suffixed name) joins the differential parser-case corpus.

  Re-probed the self-hosted parser against its own two source files with
  this fix in place: it now gets past `selfhost/lexer.di`'s entire
  symbol-heavy `next_token` (previously unreachable) and fails later, on
  a bare `private` inside `class Lexer`'s body -- visibility modifiers
  are already supported inside module bodies (`compile_module`'s own
  `:private`/`:public` handling) but were never added to `compile_class`'s
  parallel loop. Confirms symbol literals were worth fixing on their own
  terms (a large, previously-invisible gap independent of this
  self-parsing investigation) while also showing self-parsing still has
  real distance left -- each probe so far has traded one blocker for the
  next rather than reaching the end.

- Self-hosting, Phase 3 follow-up (two-hundred-eighty-third slice): bare
  `private`/`public` in class bodies, and a real bridge bug this
  uncovered. `compile_class` gains the same `:private`/`:public` handling
  `compile_module` already had, scoped to the bare (mode-flag) form only
  -- native's other form, `private(name, ...)` retroactively flipping an
  already-declared method's visibility, fails explicitly rather than
  being silently mishandled, since (unlike `compile_module_include`'s own
  named-list handling, backed by `set_module_method_visibility`) no
  bridge method exists to flip an already-declared *class* method's
  visibility after the fact. Neither self-hosted source file uses the
  named form. `compile_class`'s own register budget needed the same
  relief as `parse_name` two slices ago: extracted the previously-inlined
  `include` handling into its own `compile_class_include`, mirroring
  `compile_module_include`, to make room for the two-line dispatch
  addition.

  Verifying the bare form surfaced a real, previously-undiscovered bug in
  the native `ProgramBuilder` bridge, not the self-hosted parser: calling
  a private method through an explicit `self.foo()` receiver -- allowed,
  via `run_chunk`'s own `parameter_offset==1 && recv==0` bypass in its
  `INVOKE` handler -- failed with "private method ... called with an
  explicit receiver" even though `self` correctly compiles to register 0
  on both sides. Root cause: `declare_function` always leaves the new
  function's `owner_class` at `UINT8_MAX` ("not a method"), since it runs
  before the caller knows whether the function will end up registered as
  one -- `diamond_compile`'s own `compile_definition` sets it inline once
  `current_class`/`current_module` are known, but neither
  `declare_method` nor `declare_module_method` (the bridge's own,
  separate registration step) ever did the same. Since `parameter_offset`
  derives from `owner_class` at every call site that constructs a
  `DiamondChunk` (see `run_chunk`), every method compiled through
  `ProgramBuilder` -- not just self-hosted-parser output, anything using
  the bridge directly -- silently got `parameter_offset` 0 instead of 1.
  Fixed by having `declare_method`/`declare_module_method` set
  `owner_class` themselves (the class index, and `UINT8_MAX-1` --
  `diamond_compile`'s own module-method sentinel -- respectively) once
  the target function's role is known. A private-method-via-`self` case
  (mirroring `run_chunk`'s exact bypass condition) confirms both the
  allowed and rejected paths differentially. Verified with `make
  test-all` given the fix lives in shared runtime dispatch code, not
  anything self-hosting-specific.

- Self-hosting, Phase 3 follow-up (two-hundred-eighty-fourth slice):
  default parameter values, and a genuine step toward real self-parsing.
  Investigating what it'd take for the self-hosted parser to compile a
  *real* program (not just its own differential corpus) found that no
  target program can call anything `lib/core.di` defines, since native's
  `run_source` always prepends `core.di`'s source before compiling
  (`src/main.c`) but `Parser.compile()` only ever sees a target file's
  own text. A new `parse_and_run_with_core` entry point (kept separate
  from `parse_and_run`, which the entire existing differential suite
  depends on) splices `core.di` in front the same way, threading the
  prefix length into a new `Parser#set_offset_correction` so
  `fail()`'s diagnostic offset -- computed against `expand_source`'s
  segments, which stay relative to the *unprefixed* string -- still maps
  correctly; verified by confirming a real parse error's line was
  unaffected while `mod()` (a `core.di` function) became callable.

  Exercising that path against `core.di` itself (448 lines, a real
  library rather than curated test cases) immediately found default
  parameter values entirely unported -- `array_join`'s `separator:
  String = ""`. This is a genuinely different shape of gap than
  anything else this session: it's not "one function/receiver
  `parse_name` doesn't recognize," it's a parse-time/compile-time split
  the self-hosted architecture doesn't have room for as-is.
  `parse_parameter_names` runs *before* `compile_method_body`'s
  register-scope reset (needed to know arity before `declare_function`),
  but a default is a real expression needing to compile into the
  *function's own* frame. Solved the same way `parse_string` already
  solves interpolation: `parse_parameter_names` records each default's
  start position (offset/line/column, via a new nesting-aware
  `skip_default_expression` mirroring `compiler.c`'s own
  `parameter_count` lookahead) and skips over it without parsing;
  `compile_parameter_default`, called from inside the callee's own reset
  scope, re-lexes and compiles it for real from that saved position via
  the same embedded-Lexer save/restore trick.

  That alone wasn't sufficient: `declare_function`/`declare_method`
  always passed the same value for both arity and required_arity
  (no self-hosted concept of "optional" existed before this), and
  `compile_call`'s keyword-argument machinery (`parse_keyword_call_arguments`)
  required *every* declared slot filled, with no notion that a trailing
  unfilled one might have a default. Fixed both: a new
  `required_parameter_count` helper (index of the first parameter with a
  default, matching `parse_parameter_names`' own now-enforced "required
  parameter cannot follow a default parameter" rule) feeds
  `declare_function`/`declare_method`/`declare_module_method`, and
  `parse_keyword_call_arguments` now computes the call's actual argument
  count as the highest *filled* slot's index plus one (not the full
  declared arity) -- mirroring `compiler.c`'s own `parse_call` exactly,
  down to rejecting a gap *below* the highest filled slot ("missing
  argument", since defaults compile inline conditioned on a contiguous
  argument count, not as independently re-evaluable expressions a call
  site could reach around a gap) while accepting omitted *trailing*
  slots.

  Also extracted the parameter-binding loop (`define_local` + type-check
  + now defaults) into a shared `bind_parameters`, used by both
  `compile_function_body` and `compile_method_body`: each was already
  close enough to its own 256-register ceiling that the two-line default
  addition alone overflowed both independently, the same wall hit
  repeatedly this session. A call/definition round trip (positional,
  keyword, and omitted-default forms, for both a plain function and a
  constructor) joins the differential parser-case corpus. Confirmed
  `def self.foo` module singleton methods are a separate, pre-existing,
  still-unsupported gap while probing this -- not fixed here.

- Self-hosting, Phase 3 follow-up (two-hundred-eighty-fifth slice): the
  self-hosted parser successfully compiles its own two source files
  end-to-end for the first time. Two remaining gaps, both found by
  literally running the previous slice's `core.di`-prepending probe
  against `selfhost/lexer.di` and `selfhost/parser.di` themselves and
  reading where it stopped:

  - `ProgramBuilder.new()` -- the exact construct `parse_and_run` itself
    uses -- had never been ported, unlike the other native-recognized
    DOT constructs (`File`/`Fiber`/`Regexp`/`TCPSocket`/`TCPServer`) an
    earlier slice added. Folded into the existing `dot_construct_id`/
    `parse_dot_construct_call` dispatch as a fifth case, mirroring
    `compiler.c`'s own `parse_program_builder_new_call` (no arguments,
    straight to `PROGRAM_BUILDER_NEW`).
  - `RuntimeError.new(...)` -- the exact construct `parse_and_run`'s own
    failure path uses -- turned out to share task #15's root cause
    (`find_class` only tracks classes parsed from `.di` source, and
    native's built-in exception hierarchy never is one), but that gap is
    broader than "rescue/type filters": *any* reference to a built-in
    exception class name fails the same way, including constructing one,
    which is an extremely common pattern. Fixed at the root: a new
    `find_builtin_class`, checked as `find_class`'s fallback, hardcodes
    the twelve `DiamondBuiltinClass` names to the fixed indices and
    superclass relationships `diamond_program_init` (`src/compiler.c`)
    already establishes before any `.di` source -- including
    `ProgramBuilder.new()` -- ever declares its own first class. Verified
    both a `.new()` construction and a rescue-filter match differentially
    against a real `TCPSocket.connect` failure.

  A new `selfhost/self_parse_check.di` -- pointed at the real,
  unmodified `selfhost/parser.di` (which itself `require`s
  `selfhost/lexer.di`), `lib/core.di`-prepended the same way
  `parse_and_run_with_core` handles any real target program -- reports
  `parser.compile()` succeeding, and joins `tests/parser_diff.sh` as a
  permanent regression check rather than a one-off probe. This confirms
  the self-hosted parser can now compile arbitrary real Diamond source,
  not just its own curated differential corpus -- it does not yet mean
  the self-hosted parser can *bootstrap* (compile itself and use that
  *result* to compile something else); that remains real, distinct,
  future work, and nothing here attempts it.

- Self-hosting, Phase 3 follow-up (two-hundred-eighty-sixth slice):
  `.name=(value)` writer-method calls. `compile_invoke` had no case for
  `=` following the method name at all, so calling *any* writer-suffixed
  method through an explicit receiver -- not just one generated by
  `attr_writer`/`attr_accessor` -- was unreachable; fixed by appending
  `=` to the looked-up name exactly the way `compile_method` already
  does for writer *declarations*.

  Testing this against a module-included `attr_writer` (not just a
  class one) surfaced two more pre-existing gaps in the same area,
  fixed together since both trace back to `compile_attribute_method`
  never having a module/class branch at all (only class bodies exercised
  it before this slice added `compile_module`'s own dispatch):
  - `compile_module`'s own body loop never checked
    `attribute_keyword?`, so `attr_reader`/`attr_writer`/`attr_accessor`
    inside a `module ... end` failed to parse at all (one line, mirroring
    `compile_class`'s existing dispatch).
  - Once dispatched, `compile_attribute_method` unconditionally used
    `declare_field`/`GET_IVAR`/`SET_IVAR` and `@current_class_method_names`
    -- all class-only -- regardless of context. Now branches exactly the
    way `compile_ivar_read`/`compile_ivar_write`/`compile_method` already
    do: `declare_module_field`/`GET_IVAR_NAME`/`SET_IVAR_NAME` and the
    current module's own method-name list when compiling inside a module
    with no enclosing class.

  A class writer call and a module-included reader/writer pair (matching
  an existing `tests/cases` fixture's own shape) join the differential
  parser-case corpus. `def self.foo` module singleton methods remain a
  separate, still-open gap.

- Self-hosting, Phase 3 follow-up (two-hundred-eighty-seventh slice):
  `def self.foo` singleton methods (class and module) and
  `redefine_method`, closing out two tasks that turned out to share one
  root cause. Bigger in scope than this session's other slices: unlike
  every other gap found, native's own registration path for a directly
  declared singleton (`compile_definition`'s `module_singleton` branches,
  `src/compiler.c`) has no bridge equivalent at all -- `export_module_method`
  exists but only *re-exports an already-declared regular method*
  (the `module_function :name` syntax), a genuinely different mechanism.
  Added two new bridge methods, `declare_class_singleton_method`/
  `declare_module_singleton_method`, mirroring `declare_method`'s own
  shape but writing into `singleton_methods[]` and -- matching
  `compile_definition`'s `module_singleton` branches exactly -- never
  touching `owner_class`, since a directly-declared singleton never
  reserves register 0 for an implicit self the way an ordinary method
  does.

  On the parser side: `compile_method` now recognizes a leading `self.`
  (only where `current_class`/`current_module` make it meaningful,
  matching native's own guard), threads a `module_singleton` flag through
  arity computation (no implicit-self slot), `compile_method_body`
  (skips reserving register 0), and registration (one of four targets:
  class instance/singleton, module instance/singleton -- extracted into
  `register_compiled_method`/`duplicate_method_name?`/
  `existing_method_names` rather than inlining a four-way branch, given
  how tight `compile_method`'s own register budget already was after
  the default-parameter-values slice).

  Calling a class singleton (`Klass.foo(...)`) needed its own
  lookup path from scratch: `parse_name`'s `class_entry`+DOT branch
  unconditionally assumed `.new`, exactly the `redefine_method` blocker
  from task #16. Fixed by extending `@classes`' own tuple with a fourth
  element (each class's singleton-method descriptors, mirroring how
  `@modules` already carries several such lists) and a one-token
  lookahead (`compile_class_dot_call`, mirroring
  `keyword_argument_ahead?`'s own cloned-lexer peek) that checks for
  `redefine_method` by name, then a matching descriptor, before falling
  back to `.new`. A shared `emit_singleton_call` (parameterized by
  `needs_receiver`, native's own distinguishing field: true only for a
  `module_function` export, which wraps a self-reserving regular method
  and so still needs a leading unused receiver slot) now backs both the
  class and module singleton call paths. `redefine_method` itself was a
  small, final addition once this dispatch groundwork existed --
  `compile_redefine_method_call`, straight to the existing
  `REDEFINE_METHOD` opcode, no bridge changes needed.

  Verifying `redefine_method` against its real usage pattern (both
  existing `tests/cases` fixtures: a nested `def` inside a class
  singleton method, referenced bare and returned as the replacement
  closure) surfaced a distinct, separately-documented, pre-existing
  limitation instead of a bug in this slice: `compile_method_body`'s own
  comment already says closures aren't supported inside a method body
  this round ("methods don't participate in `@function_nesting_depth` at
  all"). Confirmed `redefine_method`'s own mechanics are correct despite
  this by testing a callable built the *supported* way (a closure nested
  in a top-level function, not a method) -- native and the self-hosted
  parser reject it with an identical error and location
  ("redefine_method callable must be a method of 'Shape'"), which joins
  `tests/parser_diff.sh` as a differential case. The full success-path
  scenario needs that separate closures-in-methods gap closed first.

- Self-hosting, Phase 3 follow-up (two-hundred-eighty-eighth slice):
  closures nested inside a method body, closing the gap the previous
  slice's `redefine_method` verification deliberately left open.
  `compile_definition`'s `at_top_level` check only ever looked at
  `@function_nesting_depth`, which `compile_method_body` never touched --
  so a `def` written directly inside a method body was silently
  miscompiled as a genuine *top-level* function (registered into
  `@functions`, no local binding created for its name), not rejected
  outright. The bare, no-parens reference to it a line later (the shape
  every real usage of this pattern takes: capture the nested `def` as a
  value, don't call it) then failed with "undefined local variable" --
  looking like an unsupported-nesting error but actually a scoping bug.
  Fixed by having `compile_method_body` increment/decrement
  `@function_nesting_depth` around the body exactly like
  `compile_function_body` already does, so the existing nested-closure
  machinery (`enclosing_locals`/`emit_closure`/the `>=2` one-level cap)
  applies unchanged.

  Getting the actual redefine_method success path working (not just
  correct rejection) needed one more piece, found by disassembling
  native's own bytecode for `tests/cases/legacy_0093.di`'s
  `square_area`/`square_area_patch` pair rather than guessing: a `def`
  nested inside a method or module-method body reserves register 0 for
  its *own independent* `self` -- unconditionally, regardless of
  nesting -- rather than capturing the enclosing method's self as a
  lexical binding. `compiler.c`'s `compile_definition` applies this
  purely from `current_class`/`current_module` context, the same
  reservation an ordinary (non-nested) method gets, which is why the
  resulting closure can later be installed via `redefine_method` and
  invoked normally against any receiver, capturing zero variables.
  Ported as `compile_definition`'s new `self_offset` (bumps the declared
  arity/required-arity by one and widens `compile_function_body`'s
  parameter-binding offset to match, mirroring `compile_method_body`'s
  own `index_offset` exactly).

  This self-reservation has one consequence `declare_function` alone
  can't produce: `REDEFINE_METHOD`'s VM-side dispatch checks the
  replacement closure's `owner_class` against the target class operand,
  and a plain `declare_function` call always leaves `owner_class` at
  `UINT8_MAX` ("not a method"), since the nested closure is never
  registered as a *named* class method the way `declare_method` would
  set it as a side effect -- it stays a bare `Closure` value, known only
  by the local variable holding it, until `redefine_method` installs it.
  Needed a new bridge method, `set_function_owner_class` (a direct
  `(Int function_index, Int owner_class)` setter, no name/duplicate
  bookkeeping at all, unlike every other `declare_*` bridge method),
  called from `compile_definition` right after `declare_function` when
  `self_offset` is set, passing either the enclosing class index or the
  same `UINT8_MAX-1` module-method sentinel `declare_module_method`'s
  bridge already uses.

  `tests/cases/legacy_0093.di`/`legacy_0094.di` now match natively
  end-to-end through the self-hosted parser (confirmed by hand, both
  outside the differential harness's scalar-only `run` limitation), and
  the redefine_method differential block in `tests/parser_diff.sh` was
  upgraded from asserting the rejection message to asserting the full
  success-path output matches byte-for-byte. Calling the nested closure
  *directly* rather than handing it to `redefine_method` (e.g.
  `helper()` right after `def helper()...end` inside a method body)
  still fails identically on both compilers with "wrong number of
  arguments" -- confirmed deliberately, not just left alone: the
  self-reservation means the closure's own declared arity always
  expects an explicit receiver-shaped first argument that no bare
  `CALL_CLOSURE` site supplies, a real property of native's own design
  rather than a self-hosted gap, matching every real fixture's own usage
  (bare reference, immediately handed to `redefine_method`, never
  called directly).

- Self-hosting, Phase 3 follow-up (two-hundred-eighty-ninth slice): a
  full differential sweep of all 820 `tests/cases/*.di` fixtures against
  the self-hosted parser (`parser_run_with_core.di`, prepending
  `lib/core.di` the way native always does), not just the curated
  `parser_cases/` corpus -- a systematic search for remaining gaps
  rather than the reactive one-at-a-time discovery every prior slice
  used. 820 total: 469 exact matches, 307 hitting the differential
  harness's own known `ProgramBuilder#run` scalar-only-result limitation
  (not real gaps -- confirmed separately for a sample via
  `parser.error_message()`, which reported "PARSED OK"), and 85 genuine
  mismatches at the start, worked down to 44 by the fixes below. Six new
  permanent `parser_cases/` fixtures (`operator_methods.di`,
  `generic_methods.di`, `yield_expression.di`,
  `attr_type_annotations.di`, `class_singleton_inheritance.di`,
  `module_include_qualified.di`) lock in everything fixed this slice,
  bringing the differential corpus to 223 cases.

  Two small pre-existing robustness bugs, unrelated to any single
  feature: `resolve_type_name` had no case for `Symbol` or `Sized`
  (`Type::SIZED` wasn't even defined -- only `SYMBOL` was, from an
  earlier slice), and `compile_function_body`'s two `return_type != nil`
  branches called `@builder.set_return_type` unconditionally instead of
  guarding it with `unless @failed` the way `compile_method_body`
  already does -- so any unresolvable return-type annotation crashed
  the native bridge (`ProgramBuilder#set_return_type arguments must be
  (Int, Int)`) instead of failing cleanly with "unknown type
  annotation". Both were one- or two-line fixes once traced.

  The largest single gap: operator method definitions (`def +(other)`,
  `def ==(other)`, `def <(other)`, etc.) inside a class, and the
  matching signature form inside an `interface` body -- entirely
  unported, accounting for the majority of the 85 initial mismatches.
  `compiler.c`'s `compile_definition` recognizes nine operator tokens as
  legal method names wherever an identifier is otherwise required
  (deliberately excluding unary minus, which is named `negate`, an
  ordinary identifier); ported as a shared `operator_method_token?`
  helper used by `compile_method`, `compile_interface_method`, and
  `compile_definition` itself (the last purely to produce the same
  "operator methods can only be defined inside a class" rejection
  native gives for a top-level `def +(a, b)`, rather than falling
  through to the generic "expected function name" message). Fixing this
  surfaced that `compile_method` never supported generic type variables
  at all (`def wrap[T](value: T) -> Array[T]`) -- ported alongside it
  since both gaps produced the same "expected '(' after function name"
  symptom in the sweep, mirroring `compile_definition`'s own
  `parse_type_variables`/`set_type_variables` handling exactly.

  `yield` was entirely unported (`Opcode::YIELD = 67`, one `parse_prefix`
  case): a bare `yield`, `yield(value)`, or a Fiber-suspending
  expression whose result is used, straight to the `YIELD` opcode
  exactly like `compiler.c`'s own `compile_yield`.

  `attr_reader`/`attr_writer`/`attr_accessor` never accepted an optional
  `: Type` annotation after the field name (`attr_accessor value: Int`)
  -- native's `compile_attribute_named` emits a `CHECK_TYPE` on the
  writer's incoming value (before `SET_IVAR`) or the reader's fetched
  value (after `GET_IVAR`, before `RETURN`) when one is given, and sets
  the underlying function's parameter/return type set for consistency
  with type-checked ordinary methods. Ported as an extra
  `type_annotation` parameter threaded from `compile_attribute` through
  `compile_attribute_method`.

  Two class-level lookups were missing a superclass walk / lexical
  fallback already present elsewhere in the port: a directly-declared
  class singleton method (`def self.foo`) is resolved at compile time
  against the class's own descriptor list only, so `Child.answer()` for
  a `Parent.answer` singleton failed with "undefined class singleton
  method" -- fixed with a new `find_class_singleton_descriptor` helper
  that walks `class_entry[2]` (superclass index) via a new
  `find_class_by_index`, mirroring `compiler.c`'s own singleton lookup
  loop. Separately, `compile_class_dot_call`'s one-token lookahead never
  checked for a writer's trailing `=` before searching descriptors, so
  `Box.value=(42)` against a `def self.value=(...)` singleton was
  misread as `Box.new` and rejected with "expected 'new' after class
  name" -- fixed by peeking a second token, mirroring `compiler.c`'s own
  `singleton_call_name_equals`. And `include Greetings` inside a class
  nested in `module Outer` failed with "undefined module" when
  `Greetings` was itself declared inside `Outer` (stored as
  `Outer::Greetings`) -- fixed with the same one-level
  `@current_module_name`-prefixed fallback already used by
  `parse_optional_superclass`/`find_class`, not native's full
  multi-level scope walk (`find_module`), since nothing in this port's
  existing corpus nests modules more than one level deep.

  Remaining categorized gaps from the sweep, deliberately left for a
  future slice rather than expanding this one further: explicit type
  arguments at call sites (`Klass.method[Type](...)`, needs
  `CALL_TYPED`/`INVOKE_TYPED` wiring at three call sites);
  `alias_method` (needs a new native bridge to copy an already-declared
  `DiamondMethod` under a new name, the same shape as this session's
  `declare_class_singleton_method` addition); named `private`/`public`
  visibility targets for classes (`private foo, bar`, retroactively
  flipping already-declared methods -- modules already have
  `set_module_method_visibility` for this; classes have no equivalent
  bridge yet); and several keyword-argument compile-time diagnostics
  (duplicate keyword, gap past a default, unknown name, positional after
  keyword) that native rejects at compile time but the self-hosted
  parser currently accepts and fails differently at runtime instead.

- Self-hosting, Phase 3 follow-up (two-hundred-ninetieth slice): closed
  every gap the previous slice's sweep catalogued, then kept sweeping --
  the differential corpus grew from 223 to 231 cases and the
  `tests/cases` mismatch count fell from 44 to 13 (all 13 confirmed
  harmless: diagnostic-format-only differences for intentional negative
  tests, the documented one-level-nesting scope cut, and two newly
  found deep gaps deliberately deferred -- see below).

  Keyword-argument diagnostics (`multiple values for the same
  argument`, `missing argument`, `wrong number of arguments`) pointed
  at the wrong column: `self.fail` always reports `@current`'s
  position, but `compiler.c` anchors these specific messages to the
  call's own function-name span, captured *before* the offending
  argument is even parsed. Added `fail_at(start, line, column,
  message)` alongside the existing position-implicit `fail`, threaded
  a saved name/keyword token through `compile_call`/
  `parse_keyword_call_arguments`/`compile_alias_method` wherever native
  anchors a diagnostic somewhere other than "wherever parsing currently
  is" -- six new `parser_error_cases` pairs lock in exact column
  matches now, not just matching message text.

  Named `private`/`public` visibility targets for classes
  (`private foo, bar`, retroactively flipping already-declared
  methods' visibility) needed a new bridge, `set_class_method_visibility`,
  mirroring `set_module_method_visibility` exactly. `alias_method`
  needed two more, `alias_class_method`/`alias_module_method`, copying
  an already-declared `DiamondMethod` struct under a new name (the
  parser pre-checks source-exists/alias-not-taken itself first, the
  same reason `duplicate_method_name?` guards every `declare_method`
  call -- a bridge failure has no rescue anywhere in this parser and
  would crash the whole compile attempt instead of a clean
  `error_message()`). Both features share one subtlety compiler.c's own
  `compile_visibility` already handles: a writer target (`private
  value=`) is registered under `"value="`, found only by peeking one
  token ahead for a trailing `=`, not by the bare name alone.

  Explicit generic call arguments (`obj.method[Type](...)`,
  `Klass.method[Type](...)`) were wired at the two remaining call
  sites -- `compile_invoke` (new `Opcode::INVOKE_TYPED = 42`) and both
  class/module singleton call paths (reusing `CALL_TYPED`) -- extracted
  into a shared `parse_explicit_type_arguments` helper alongside
  `compile_call`'s own preexisting (and left untouched) copy. Unlike
  `compile_call`, none of these three validate the parsed count against
  a statically-known `type_variable_count`: `compile_invoke` can't (the
  method is only resolved at INVOKE time against the receiver's runtime
  class, mirroring `compiler.c`'s `parse_invoke` exactly), and the two
  singleton paths don't carry `type_variable_count` in their descriptor
  tuples at all -- a deliberate simplification, not a bug, since the VM
  itself still enforces the count at runtime either way.

  Sweeping the newly-unblocked syntax against real usage surfaced three
  genuine, pre-existing runtime bugs, none related to this slice's own
  additions:
  - `bind_parameters` emitted a parameter's `CHECK_TYPE` *before* its
    default-value fallback, so any typed parameter with a default
    (`greeting: String = "hi"`, including `lib/core.di`'s own
    `array_join`) rejected every call that actually relied on the
    default -- the omitted argument's register still held the VM's
    ordinary zero-init `Nil` at that point, not the eventual fallback
    value. `compiler.c`'s own parameter loop parses the default first
    and only calls `emit_type_check` afterward; reordered to match.
  - A `module_function`-exported method's call-site arity check used
    exact equality against its declared arity, with no way to represent
    "optional" at all -- the exported descriptor tuple never carried
    `required_arity` in the first place (unlike the directly-declared
    `def self.foo` descriptor, which always did). Added it as a fifth
    element and switched the check to the same range test the
    directly-declared path already used.
  - `attr_predicate` was entirely unported, previously reasoned to be
    safely skippable as "unused by either of the self-hosted compiler's
    own source files" -- true for the compiler's own sources, but not
    for arbitrary target programs, several of which use it. Confirmed
    against `compile_attribute_named` that it really is just a reader
    whose method name gets a `?` suffix (no Bool-conversion behavior
    anywhere), then ported as a third suffix case alongside the
    existing writer-suffix (`=`) handling.

  Two deep gaps found and deliberately left open rather than expanding
  this slice further, both confirmed via minimal repros isolated from
  the original failing fixtures, not guessed at:
  - Runtime constraint enforcement for a generic collection's element
    type (`Array[T]`/`Hash[K, V]`) doesn't propagate through the
    self-hosted parser's compiled output the way it does natively.
    Assumed at the time to need deep VM-level tracing; see two slices
    ahead for the actual (parser-side, not VM-side) root cause. (Resolved.)
  - Every native builtin-name recognition (`print`/`puts`/`gets`,
    `File.open`, the Math functions, `Fiber.new`, `Regexp.new`,
    `TCPSocket`/`TCPServer`, `ProgramBuilder`, `chr`/`to_f`/`to_i`/
    `to_sym`) is gated behind `find_local(...)<0 && find_function(...)<0`
    in `compiler.c` -- a user-defined top-level function can shadow any
    of them. Assumed at the time to need a wide, mechanical fix across
    every dispatch site; see the next slice for what auditing each one
    individually actually found. (Resolved.)

- Self-hosting, Phase 3 follow-up (two-hundred-ninety-first slice):
  auditing every builtin-name dispatch site the previous slice flagged
  found the gap was narrower than assumed: `is_file_open_target`,
  `dot_construct_id` (`Fiber`/`Regexp`/`TCPSocket`/`TCPServer`/
  `ProgramBuilder`), `is_builtin_scalar_target` (`gets`/`chr`/`to_f`/
  `to_i`/`to_sym`), and `is_math_unary_target`/`is_math_binary_target`
  already checked `find_function(name) == nil` themselves -- only the
  two literal `name == "puts"`/`name == "print"` checks at the top of
  `parse_name_call` never consulted it at all, unconditionally treating
  either name as the builtin regardless of a same-named top-level
  `def`. (Local shadowing was already handled globally: `parse_name_call`
  dispatches to `compile_closure_call` for any non-nil `local` before
  reaching any of these checks.) Fixed with the same one-line guard the
  other five checks already had. `tests/cases/legacy_0330.di`'s
  `def print(x) = "shadowed"` now matches natively (confirmed via a
  boolean-returning wrapper, sidestepping the differential harness's
  own scalar-only `run` limitation the same way prior slices did), and
  a new `builtin_name_shadowing.di` case joins the corpus.

- Self-hosting, Phase 3 follow-up (two-hundred-ninety-second slice): the
  generic-collection-constraint gap turned out to be a self-hosted
  parser bug, not the deep VM-level mystery it looked like -- found by
  temporarily instrumenting `run_chunk`/`value_matches_member` with
  `DIAMOND_DEBUG_GENERICS`-gated `fprintf`s (removed afterward, `vm.c`
  ends this slice byte-identical to how it started) and diffing native's
  trace against the self-hosted parser's own on the same input. Both
  correctly *inferred* `pair[K, V]`'s type-variable bindings from its
  arguments (`[infer]` lines matched exactly); native then logged a
  `[member] id=Hash argument_set=... second_argument_set=...` entry for
  the `-> Hash[K, V]` return-type check that the self-hosted trace never
  produced at all -- the CHECK_TYPE for the return value was simply
  never being emitted.

  Root cause: `emit_type_check`'s "skip the runtime check, we already
  statically know this register's type" optimization
  (`annotation_accepts_type?`) only ever compared *bare* type IDs -- a
  hash literal's fact is plain `Type::HASH`, which trivially matches
  `Hash[K, V]`'s own bare id, so the optimization fired and skipped the
  check entirely, silently discarding the element-type constraint
  along with it. `compiler.c`'s own `emit_type_check` guards against
  exactly this: it forces the check whenever the matched union member
  carries an `argument_set` (an element constraint), bare-type match or
  not -- collection element constraints, generic or concrete alike,
  never get to ride on the fact-based skip. Ported as one extra
  condition in `annotation_accepts_type?` (the matched member must have
  no `argument`/`second_argument` for the skip to apply at all), not a
  parallel type-variable-detection pass -- confirmed sufficient since a
  bare type-variable return (`-> K` with no wrapping collection) can
  never coincidentally match a concrete fact in the first place (facts
  only ever hold concrete `Type::*` ids, never a 96+ type-variable id),
  so it already fell through to the runtime check before this fix too.

  `tests/cases/legacy_0154.di` (the `Hash[K, V]` case) and the earlier
  `Array[T]` repro from two slices back both match natively now. A new
  `generic_collection_constraints.di` case covers both container
  shapes.

- Self-hosting, Phase 3 sub-phase 4 (twenty-second slice): array literals.
  The self-hosted parser now lowers empty and populated array literals with the
  VM's contiguous-register `ARRAY` instruction, enforces the 32-element limit,
  and records the exact collection fact. A runtime length check brings the
  parser harness to 71 cases.

- Self-hosting, Phase 3 sub-phase 4 (twenty-third slice): hash literals. Empty
  and populated hashes now lower through the VM's interleaved key/value
  register layout with the 16-entry parser limit and an exact `Hash` fact. A
  runtime length check brings the parser harness to 72 cases.

- Self-hosting, Phase 3 sub-phase 4 (twenty-fourth slice): indexed reads.
  Postfix brackets now lower to `INDEX_GET` for arbitrary receivers, including
  chained expressions, while local indexing is disambiguated from explicit
  generic calls. A runtime array lookup brings the parser harness to 73 cases.

- Self-hosting, Phase 3 sub-phase 4 (twenty-fifth slice): array index result
  contracts. Indexing a value declared as `Array[T]` now attaches `T` to the
  result and records an exact fact for single-member element contracts. A
  typed integer lookup and return brings the parser harness to 74 cases.

- Self-hosting, Phase 3 sub-phase 4 (twenty-sixth slice): hash index result
  contracts. Indexing `Hash[K, V]` now declares the destination as `V | Nil`,
  reflecting missing-key behavior and feeding the existing nil-sensitive
  branch analysis. A narrowed typed hash lookup brings the parser harness to
  75 cases and completes the planned collection-index propagation path.

- Self-hosting, Phase 3 sub-phase 4 (twenty-seventh slice): indexed writes.
  Local array and hash targets now recognize bracket assignment, including
  nested index expressions and captured receivers, and lower to `INDEX_SET`.
  A write-then-read array case brings the parser harness to 76 cases.

- Self-hosting, Phase 3 sub-phase 4 (twenty-eighth slice): logical-negation
  facts. Prefix `!` and `not` destinations now carry their guaranteed `Bool`
  type through assignments and returns. A typed negation function brings the
  parser harness to 77 cases.

- Self-hosting, Phase 3 sub-phase 4 (twenty-ninth slice): numeric-negation
  facts. Unary minus now preserves known `Int` and `Float` facts on its result,
  while unknown operands remain conservative. A typed negative integer return
  brings the parser harness to 78 cases.

- Self-hosting, Phase 3 sub-phase 4 (thirtieth slice): equality result facts.
  `==` and `!=` destinations now retain their guaranteed `Bool` type alongside
  any nil-narrowing metadata. A typed equality predicate brings the parser
  harness to 79 cases.

- Self-hosting, Phase 3 sub-phase 4 (thirty-first slice): ordered-comparison
  facts. `<`, `<=`, `>`, and `>=` destinations now carry their guaranteed
  `Bool` type through local propagation and typed returns. A typed comparison
  predicate brings the parser harness to 80 cases.

- Self-hosting, Phase 3 sub-phase 4 (thirty-second slice): arithmetic result
  facts. Known integer operands retain `Int`, mixed known numeric operands
  promote to `Float`, and string concatenation retains `String`, matching the
  C compiler while leaving VM quickening independent. A typed arithmetic
  return brings the parser harness to 81 cases.

- Self-hosting, Phase 3 sub-phase 4 (thirty-third slice): short-circuit result
  joins. Logical `and`/`or` expressions now retain an exact fact or full
  declaration only when their left and right outcomes agree. A typed Boolean
  conjunction brings the parser harness to 82 cases.

- Self-hosting, Phase 3 sub-phase 4 (thirty-fourth slice): homogeneous array
  literal contracts. Non-empty primitive arrays whose elements share an exact
  fact now infer `Array[T]`, feeding immediate indexed reads and aliases. A
  typed literal lookup brings the parser harness to 83 cases.

- Self-hosting, Phase 3 sub-phase 4 (thirty-fifth slice): homogeneous hash
  literal contracts. Non-empty primitive hashes with uniform key and value
  facts now infer `Hash[K, V]`, so immediate lookups receive `V | Nil` and can
  use nil narrowing. A typed literal lookup brings the parser harness to 84
  cases.

- Self-hosting, Phase 3 sub-phase 4 (thirty-sixth slice): conservative loop
  exits. `while` and `until` now restore entry facts after their body because
  the body may execute zero times, preventing branch-local assignments from
  becoming unconditional post-loop proofs. A zero-iteration typed-local case
  brings the parser harness to 85 cases and completes sub-phase 4.

- Self-hosting, Phase 3 sub-phase 5 (first slice): explicit raises. `raise`
  with a value now lowers to the VM's `RAISE` instruction in entry, function,
  closure, and method bodies; bare re-raise remains reserved for rescue
  context. A compiled raising function brings the parser harness to 86 cases.

- Self-hosting, Phase 3 sub-phase 5 (second slice): catch-all rescue. `begin`
  expressions with one unfiltered `rescue` now register and pop VM rescue
  handlers on the appropriate paths and merge their result value. A raised
  string recovered as an integer brings the parser harness to 87 cases.

- Self-hosting, Phase 3 sub-phase 5 (third slice): rescue bindings. A catch-all
  clause may bind its exception register to a local name scoped to the rescue
  body, matching the native compiler without an extra move. Returning the
  rescued integer brings the parser harness to 88 cases.

- Self-hosting, Phase 3 sub-phase 5 (fourth slice): typed rescue filters.
  Rescue clauses now accept up to eight `|`-separated runtime types, patch
  those IDs into the VM handler, and narrow a single-type exception binding.
  An integer-filtered rescue brings the parser harness to 89 cases.

- Self-hosting, Phase 3 sub-phase 5 (fifth slice): ensured rescue expressions.
  Rescued `begin` forms now install an outer ensure handler and run optional
  `ensure` bodies through `RUN_ENSURE`/`END_ENSURE`, covering normal, rescued,
  escaping, and return unwinds. A mutating ensure case brings the parser
  harness to 90 cases.

- Self-hosting, Phase 3 sub-phase 5 (sixth slice): bare re-raise. Rescue bodies
  now expose their active exception register to `raise` without an operand,
  restoring any outer rescue context across nesting. A nested re-raise caught
  by an outer clause brings the parser harness to 91 cases.

- Self-hosting, Phase 3 sub-phase 5 (seventh slice): rescue `else`. An optional
  `else` body now replaces the result only after normal protected completion;
  rescued paths skip it and both paths still enter `ensure`. A normal-path
  else result brings the parser harness to 92 cases.

- Self-hosting, Phase 3 sub-phase 5 (eighth slice): rescue retry. Rescue bodies
  now expose the protected handler-installation offset to `retry`, with nested
  contexts restored on exit. A two-attempt recovery brings the parser harness
  to 93 cases.

- Self-hosting, Phase 3 sub-phase 5 (ninth slice): exact rescue-result joins.
  A `begin` expression now retains an exact destination fact when its normal
  body and rescue body produce the same type. A typed integer recovery brings
  the parser harness to 94 cases.

- Self-hosting, Phase 3 sub-phase 5 (tenth slice): declared rescue-result
  joins. When normal and rescued values carry the same full contract, the
  `begin` destination now retains it for subsequent nil and `is` narrowing. A
  nominal union recovery brings the parser harness to 95 cases.

- Self-hosting, Phase 3 sub-phase 5 (eleventh slice): ensure-only expressions.
  Rescue handlers now begin disabled and are enabled only by an actual rescue
  clause, allowing `begin ... ensure ... end` to run cleanup while preserving
  normal results and propagating exceptions. A normal cleanup case brings the
  parser harness to 96 cases.

- Self-hosting, Phase 3 sub-phase 5 (twelfth slice): rescue-filter contracts.
  Typed rescue bindings now retain the full filtered union, not just a
  single-member exact fact, enabling `is` and complementary narrowing inside
  multi-type handlers. A two-type predicate brings the parser harness to 97
  cases.

- `lsp/`, a v1 (diagnostics-only) Language Server Protocol implementation
  — its own standalone binary (`build/diamond-lsp`, `lsp/*.c`, deliberately
  not part of the `diamond` runtime or its release, the same relationship
  `facet` has to it), speaking real `Content-Length`-framed JSON-RPC over
  stdio. `initialize`/`textDocument/didOpen`/`didChange`/`didClose`/
  `shutdown`/`exit`, each open document recompiled through
  `diamond_compile` and published as a `textDocument/publishDiagnostics`
  notification. Written in C, not Diamond — Diamond has no JSON support
  or byte-precise stdio reads yet, both of which the wire protocol needs
  — and not derived from any other language's LSP (Ruby's came up once as
  a conversational reference point for a possible future syntax-highlighting
  grammar, nothing more). Full design and scope, including what's
  deliberately not built yet (hover, go-to-definition, completion,
  cross-file `require` resolution, incremental sync), in `docs/lsp.md`.

- `editors/vscode/`, a minimal syntax-highlighting-only VS Code extension
  (`package.json` + `language-configuration.json` +
  `syntaxes/diamond.tmLanguage.json`, no compiled code, no build step). The
  TextMate grammar is built directly from `src/lexer.h`/`src/lexer.c`'s own
  token list — not derived from or dependent on any other language's
  grammar — including the same colon disambiguation the real lexer uses to
  tell a Symbol literal (`:name`) apart from a type-annotation or
  hash-literal colon glued onto a preceding identifier/`)`/`]`/`}`/`"`.
  Every regex verified against real method/class signatures pulled from
  this repo's own fixtures (operator-overload defs, `def self.name`,
  generic `def wrap[T]`, writer defs, string interpolation including a
  nested string inside `#{...}`), not just eyeballed. No `node`/`npm`
  available in this environment to run a real `vscode-textmate` tokenizer
  end-to-end, so verification stopped at regex-level correctness (Python's
  `re`, close enough to Oniguruma for the patterns used here — no
  possessive quantifiers or atomic groups) rather than a full tokenization
  trace; see `editors/vscode/README.md` for how to load it into a real VS
  Code window to check by eye. Diagnostics are `lsp/`'s job (see above),
  not this extension's — no semantic highlighting here, regex-only.

- Fixed a real bug in `lsp/`'s own diagnostics found immediately after
  shipping it: `diagnostics_compute` compiled a document's raw text
  directly, but `require` isn't a lexer/parser token at all (confirmed by
  its absence from `src/lexer.h`'s token list) -- it's resolved entirely
  by `diamond_load_program` (`src/loader.c`) as a source-level
  preprocessing step *before* `diamond_compile` ever runs, which
  `diagnostics_compute` skipped. Every document containing a `require`
  line -- a large fraction of any real multi-file project, demonstrated
  throughout this project's own `README.md` -- reported a bogus
  "undefined local variable" diagnostic on that line, regardless of
  whether the code was valid.

  Fixed by converting the document's `file://` uri to a filesystem path
  (percent-decoded; `require` resolves relative to it, dependencies read
  from disk) and calling `diamond_load_program` first, matching
  `src/main.c`'s own `run_source` exactly. Uncovered a second, deeper
  problem doing this correctly: a diagnostic's line/column, once
  `require`d content is bundled in, can only be correctly attributed
  through `DiamondSourceBundle`'s segment table -- logic that already
  existed, but only as 30-odd lines buried inside `src/main.c`'s
  file-local, stderr-writing `print_diagnostic`, with no way for a
  second caller to reuse it short of reimplementing the same
  offset/newline-counting arithmetic. Extracted into a new shared
  `diamond_resolve_diagnostic_location` (`src/compiler.h`/`.c`,
  returning data instead of printing), with `src/main.c`'s own
  `print_diagnostic` rewritten as a thin formatting layer over it —
  verified byte-for-byte behavior-preserving via the full native test
  suite and every source-map differential case in `tests/parser_diff.sh`
  before trusting the extraction, not just by inspection.

  A diagnostic that resolves to a `require`d file rather than the open
  document itself currently reports nothing for that document (not
  misattributed to the wrong file, but not shown anywhere yet either) --
  and an unresolvable `require` reports `diamond_load_program`'s own
  error text anchored at the document's start, since its message has no
  machine-parseable location to extract a precise range from. Both
  documented as known v1 scope in `docs/lsp.md`, along with three new
  `tests/lsp_test.sh` cases (a valid multi-file project publishing no
  false diagnostic, a real error on the line *after* a require reporting
  the correct line, and an unresolvable require) against a real on-disk
  temp directory.

- `fuzz/compile_fuzzer.c`, a libFuzzer harness for `diamond_compile` —
  lexer, parser, and bytecode emitter together, the exact surface a `.di`
  file's raw bytes are exposed to. Clang-only (`-fsanitize=fuzzer` is a
  Clang/LLVM feature GCC doesn't implement), so it gets its own compiler
  variable (`CC_FUZZ`) and its own from-scratch object build in the
  Makefile rather than reusing any GCC-built `.o` — libFuzzer's coverage
  instrumentation has to cover the actual compiler code it's fuzzing, not
  just the harness entry point. Deliberately compile-only, never
  `diamond_vm_run`: Diamond's real `File`/`TCPSocket`/`Regexp` bridges
  mean actually executing an arbitrary mutated program isn't safe without
  sandboxing/resource limits this harness doesn't attempt. Full reasoning,
  how to run a real (unbounded) campaign vs. the bounded regression check,
  and what's still out of scope in `docs/fuzzing.md`.

  `make test-fuzz` (`tests/fuzz_smoke.sh`) is a 20-second bounded run
  seeded from `tests/parser_cases/*.di`, wired into `test-all` as a
  regression check, not a real campaign. Verified the crash-detection path
  itself actually works (not just the happy path) with a throwaway
  always-`abort()` harness before trusting it: confirmed the script
  reports nonzero and surfaces the crashing input the same way it would
  for a real one. An initial ~23,000-run manual campaign against the seed
  corpus (well beyond the smoke test's own 20-second/~6,000-run bound)
  found nothing — expected, given the extensive existing differential
  test suite, not evidence fuzzing has nothing left to find.

- Stdlib round 3: Enumerable completeness. Eight new dot-call methods —
  `sum`, `sort`, `sort_by`, `reject`, `find`, `each_with_index`, `min`,
  `max` — on top of round 1's `array_sort`. `sort`/`sort_by`/`min`/`max`
  reuse native `<`/`>` (an insertion sort, matching `array_sort`'s own
  algorithm choice) rather than the old Int-only restriction, so they
  automatically work on any user class with `<`/`>` overloaded (see this
  file's operator-overloading entry) with zero extra VM code — verified
  against a `Box` class defining only `<`/`>`. `array_sort` itself stays
  Int-only and untouched: `tests/run.sh` has a standing assertion that
  `array_sort([1, "bad", 2])` rejects a mixed array, so widening its
  signature would have been a breaking change, not just an addition; the
  new methods are separate functions instead.

  Scoped narrower than first planned, for two different reasons. First,
  correctness: `sum`/`sort`/`sort_by`/`reject`/`find`/`each_with_index`/
  `min`/`max` are Array-only, unlike `select`/`count`/`any?`/`all?`/`map`/
  `reduce`, which all operate through `.each()` and so work on Hash values
  too. Second, and the harder constraint: `DIAMOND_MAX_FUNCTIONS` (256) is
  not a resizable soft limit — function indices are stored as a single
  byte throughout the bytecode format (`CONSTANT`/`CALL` operands,
  `DiamondMethod`/`DiamondClosure.function_index`), so raising the
  constant would silently wrap indices past 256 rather than actually
  expand capacity (see the constant's own comment in `src/vm.h`). The
  self-hosted parser's own bootstrap self-compile (`lib/core.di` +
  `selfhost/lexer.di` + `selfhost/parser.di`, the largest real program
  this codebase compiles) was already at 243 of 256 before this round.
  The first version of this slice — Hash support via `.each()` + nested
  closures for `sum`/`reject`/`find`, mirroring `enumerable_select`'s
  existing idiom, plus all eight methods also exposed through `module
  Enumerable` for arbitrary-class `include` — pushed that to 259 and
  broke the bootstrap check outright. Fixed by rewriting `sum`/`reject`/
  `find` as direct Array `while`-loops (no `.each()`, no nested closures,
  matching `array_compact`'s style) instead of the Hash/Array-dispatching
  pattern, and by dropping all eight from `module Enumerable`: the
  Array/Hash dot-call path never goes through module method lookup at all
  (`DIAMOND_OP_INVOKE`'s hardcoded dispatch calls the top-level function
  directly), so those eight module entries only mattered for a class
  `include`-ing `Enumerable` — and since all eight are written against
  `values[index]`/`.length()`, not `.each()`, they wouldn't have worked
  for a generic each-based class anyway, unlike the original six. Cutting
  them cost nothing real and brought the bootstrap total to 251, five
  slots of headroom rather than negative three. Caught by actually running
  `make test-all` (not just `make test`) before considering this done —
  the failure only showed up in the self-hosted parser's own bootstrap
  compile, not in the ordinary test suite, which doesn't stress the
  function-count budget anywhere near this hard.

- Widened function indices from a single byte to 16 bits, raising
  `DIAMOND_MAX_FUNCTIONS` from 256 to 512. The prior 256 cap was a real
  architectural ceiling, not a struct-sizing choice (see its own comment
  in `src/vm.h`): `CALL`/`CALL_TYPED`/`CLOSURE`'s function-index operand
  and `DiamondMethod`/`DiamondClosure.function_index` were all
  `uint8_t`, so raising the constant alone would have silently wrapped
  indices past 256 rather than actually expanding capacity. Immediately
  relevant, not preemptive: the self-hosted parser's own bootstrap
  self-compile was already at 251/256 after the Enumerable-completeness
  round just above, and the next self-hosting phase (exceptions,
  modules, `require`) was always going to need more than five slots of
  headroom.

  Three call sites carry the operand in the actual bytecode format
  (`CALL`/`CALL_TYPED`/`CLOSURE`); everywhere else `function_index` is a
  plain struct field set from C code, not a bytecode read, so only those
  three needed a real format change. Widened to big-endian 16-bit,
  matching the existing `JUMP`/`JUMP_IF_TRUE`/`JUMP_IF_FALSE` operand
  convention exactly (`patch_jump`'s high-byte/low-byte split) rather
  than inventing a new encoding — `compiler.c` gained one
  `emit_function_index` helper used at all three emission sites,
  `vm.c`'s interpreter loop gained a matching `READ_SHORT` macro
  alongside the existing `READ_BYTE`, and `disassemble.c`'s
  `CALL`/`CALL_TYPED`/`CLOSURE` cases grew their `require_bytes` operand
  counts by one and read/print the combined value instead of a single
  byte. 512 is deliberately not the full `uint16_t` range: `DiamondFunction`
  is roughly 152KB (its own fixed-size code/constant/string/type-set
  arrays), so `DIAMOND_MAX_FUNCTIONS` dominates every heap-allocated
  `DiamondProgram`'s size almost linearly; 512 keeps the whole struct
  in the "tens of MB" range this codebase already accepts (`sizeof
  (DiamondProgram)` went from 45MB to roughly 83MB) while giving the
  self-hosted bootstrap real headroom again.

  The self-hosted parser (`selfhost/parser.di`) needed the identical
  format change on its own emission side, independent of the native
  compiler: it hand-assembles its own `CALL`/`CALL_TYPED`/`CLOSURE`
  bytes through the `ProgramBuilder` bridge, so it has its own
  `emit_function_index` (mirroring `patch_jump`'s existing `/256`,
  `mod(_, 256)` split, already used there for jump targets) at its three
  call sites — missing this would have made the self-hosted compiler
  keep emitting the old one-byte layout, silently misaligning every
  instruction after the first multi-function program's first call.
  Caught one further real regression the same way as the Enumerable
  round above: `tests/cases/program_builder_call_declared_function.di`
  hand-assembles a raw `CALL` instruction byte-by-byte via
  `ProgramBuilder#emit_byte` to test that bridge API directly, and it
  still encoded the old one-byte function-index layout — fixed by
  inserting the new high-byte operand. The two sibling
  `program_builder_run_*` fixtures were unaffected; they don't exercise
  `CALL` at all.

- `ProgramBuilder#run` can now return `String`/`Symbol`/`Bignum` results,
  not just `Int`/`Float`/`Bool`/`Nil`. Root-caused via a full differential
  sweep of `tests/cases/*.di` (824 files) through `selfhost/parser.di` +
  `ProgramBuilder#run`, well beyond the curated 233-file `parser_cases`
  corpus `tests/parser_diff.sh` normally checks: 264 of 276 real mismatches
  traced to this single restriction, dwarfing every other gap combined.
  The restriction was never an arbitrary scope cut — `run`'s bytecode
  executes in a *separate* temporary `DiamondVm` (`run_vm`, stack-frame-
  isolated per this function's own existing comment) that gets
  `diamond_vm_free`'d before returning, and that unconditionally frees
  every object it allocated, so handing back a heap-object result without
  copying it first would dangle immediately. Fixed with a new
  `copy_value_into_vm` helper that deep-copies a value out of `run_vm`'s
  heap into the caller's own heap before the free. `program_builder_
  run_rejects_object_result.di` — a hand-assembled-bytecode unit test
  whose entire point was locking in the old blanket rejection — now
  constructs a `Closure` result instead of the `String` it used before,
  since closures wrap live function/capture state that can never be
  copied this way, unlike the plain-data kinds landing here and in the
  slices immediately following.

- `copy_value_into_vm` now recurses into `Array`, so `ProgramBuilder#run`
  can return an array (including nested arrays/strings) as easily as a
  bare `String`. Each element copies through the same function, so a
  still-unsupported element (an `Instance`, say) fails the whole array's
  copy the same way a bare unsupported result would, rather than
  returning a partially-copied array. Deliberately drops the source
  array's own generic constraint metadata (`constraints[]`) rather than
  trying to carry it across — those hold pointers into the *source*
  program's type-set/class tables with the same lifetime problem as
  Instance's `->class` above, so a copied array comes back a plain,
  unconstrained one.

- `copy_value_into_vm` now handles `Hash` too, same shape as `Array`:
  recurses into both key and value for every entry via `hash_set`
  (forward-declared, since its own definition sits much later in
  `vm.c` than this function), fails the whole hash if any entry's key
  or value doesn't copy, and drops constraint metadata for the same
  reason arrays do. This closes out the object-kind side of the
  `ProgramBuilder#run` gap found two slices back — `String`/`Symbol`/
  `Bignum`/`Array`/`Hash`, recursively nested any which way, now round-trip
  through a self-hosted-compiled-and-run program correctly; only
  `Instance` and the VM/OS-resource-wrapping kinds remain out of reach,
  the former for a real lifetime reason (see above), the rest because
  they were never portable data to begin with.

- Three new `tests/parser_cases/` fixtures (`program_result_string`,
  `program_result_array`, `program_result_hash`) return a `String`/
  `Array`/`Hash` directly as the program's own top-level result, with no
  workaround needed — every *existing* fixture in this 233-file curated
  corpus ends with an explicit trailing `puts(...)`-then-`nil` specifically
  to keep its own top-level result scalar, evidently working around the
  very restriction the last three slices lifted. Verified with a full
  `tests/cases/*.di` (824 files) differential sweep through the
  self-hosted parser, well beyond the curated corpus: mismatches dropped
  from 276 (pre-copy) to 28. Of those 28, most are `Instance` results
  (the one deliberately-still-unsupported kind, expected) or already-known
  gaps; a handful are genuine, previously-invisible correctness bugs the
  old scalar-only restriction had been hiding completely, since a
  self-hosted-compiled program returning any of them could never
  previously be run at all to expose the bug:
  - `legacy_0132`: a later default parameter (`b = a + 2`) isn't
    re-evaluated when skipped, but an explicit argument supplied for it
    is silently dropped in favor of `nil` instead of being used.
  - `legacy_0121`/`legacy_0122`: interface `is` checks accept a method
    whose parameter type violates contravariance (e.g. a narrower
    `String` parameter where the interface declares `Animal`), when they
    should reject it.
  - `legacy_0156`: a generic `Array[T]` returned from a generic method
    doesn't enforce its element-type contract on a later `.push`, letting
    a wrong-typed push through instead of raising `TypeError`.
  Each becomes its own slice next.

- Fixed the first: `legacy_0132`'s wrong-value bug. Root cause was in
  `selfhost/parser.di`'s `bind_parameters`, not the default-fallback logic
  it looked like at first — `define_local` (one register per parameter)
  was interleaved with `compile_parameter_default`'s own scratch-register
  allocations (one for `ARGUMENT_PROVIDED`'s result, more for the
  fallback expression), so a *second* defaulted parameter's register
  landed wherever `@next_register` happened to be after the first
  parameter's default finished compiling, not at the fixed slot the VM's
  calling convention actually uses (`registers[index] = arguments[index]`
  at function entry, see `run_chunk`). A single defaulted parameter never
  showed the bug — nothing came after it to get bumped — which is why
  `legacy_0132` (two defaults) failed while dozens of one-default fixtures
  already in the corpus passed. Fixed by mirroring `compiler.c`'s own
  two-phase pattern exactly: reserve a contiguous register block for
  every parameter first (`parameter_base` + one `allocate_register()` per
  parameter, no binding or default-compiling yet), *then* walk the
  parameters again binding each name to its pre-reserved register and
  compiling its default/type-check. New `tests/parser_cases/
  multiple_defaulted_parameters.di` covers plain functions and a
  three-defaulted-parameter constructor (exercising the method
  `index_offset=1` path, where register 0 is already reserved for `self`
  before this function even runs) — the exact shape of function this bug
  needed and the existing corpus never had.

- Fixed the second: `legacy_0121.di`/`legacy_0122.di`'s over-permissive
  interface `is` checks. `bind_parameters` was calling `@builder.
  set_parameter_type(function_index, index + index_offset, set_index)`
  for every typed parameter — correct for `ARGUMENT_PROVIDED`'s own
  index earlier in the same function (a call-time argument position,
  which genuinely does include the receiver), wrong here: `DiamondFunction
  .parameter_type_sets` is indexed purely by declared-parameter position,
  with no slot for `self` at all, self or no self — confirmed
  independently by `attr_writer`'s own generated setter a few thousand
  lines later in this same file, which already calls `set_parameter_type
  (function_index, 0, ...)` with no offset. The `+index_offset` shifted
  every method parameter's real type into the next slot over, leaving
  slot 0 permanently `UINT8_MAX` ("untyped") for any single-parameter
  method — and an untyped actual parameter is deliberately compatible
  with *any* required interface type (see `vm.c`'s interface-satisfies
  check), so a method whose real parameter type was actually incompatible
  read as untyped instead, and untyped trivially passes. New
  `tests/parser_cases/interface_parameter_contravariance.di` locks in
  both directions this bug got wrong: a genuinely-narrower parameter type
  correctly rejected, and a genuinely-untyped parameter correctly
  accepted regardless of the interface's own declared type.

- The third audit finding, `legacy_0156.di` (a generic `Array[T]` method
  return not enforcing its element-type contract on a later `.push`),
  turned out to already be fixed — not by anything in this run, but as a
  side effect of the previous slice's `set_parameter_type` correction:
  `wrap[T](value: T)`'s own `value` parameter is typed, so the wrong-slot
  bug there was corrupting this case too, incidentally, alongside the
  interface checks it was actually found through. `annotation_accepts_
  type?` (`emit_type_check`'s compile-time fast path, skipping the
  runtime `CHECK_TYPE` when a cheaper static fact already covers an
  annotation) already had its own dedicated fix and regression test from
  earlier self-hosting work — `tests/parser_cases/
  generic_collection_constraints.di` — but only for a top-level function
  called with an *explicit* type argument (`empty[String]()`). A method
  whose generic binding comes from ordinary argument *inference*
  (`Box.new().wrap("diamond")`, no explicit `[String]`) was a genuinely
  different, untested combination that happened to also route through the
  now-fixed code path. New `tests/parser_cases/
  generic_method_inferred_array_constraint.di` closes that specific gap
  so it stays fixed regardless of what future changes touch either code
  path.

- Ran down the remaining audit findings. Two turned out not to be gaps at
  all, just an artifact of the ad-hoc full-corpus diff script this whole
  run has been driven by: it compares `$diamond "$case_file"`'s direct
  compile-error format (`path:line:col: error: message` + source snippet
  + caret) against `parse_and_run_with_core`'s own `raise RuntimeError.
  new(parser.error_message())`, which an uncaught-exception harness like
  `parser_run_with_core.di` presents completely differently (`runtime
  error: uncaught exception: RuntimeError` + a stack trace) even when the
  underlying `parser.error_message()` text is byte-identical. Checked all
  7 via `selfhost/parser_check.di`, which prints `error_message()`
  directly: the 6 `keyword_args_error_*` cases already matched natively,
  word for word, already covered by the existing `tests/parser_error_
  cases` corpus (122 cases) that compares error text correctly.
  `operator_def_outside_class_rejected.di` also already matched but had
  no dedicated coverage there yet — added `tests/parser_error_cases/
  operator_def_outside_class.di`/`.err` to close that.

  The one genuine remaining item, `deep_closure.di`'s "only one level of
  function nesting is supported" restriction, is confirmed real and
  deliberate, not a bug: `compile_definition`'s own comment traces it to
  the self-hosted parser's *own* compile-time register budget (this
  method itself needed splitting into `parse_parameter_names`/
  `compile_function_body`/`emit_closure` earlier in Phase 3 specifically
  because a single-method version exhausted 256 registers), not any limit
  in the bytecode format or VM — native's own nested closures have no
  depth limit at all (see this file's own "Nested functions are
  first-class closures" line). Left as-is rather than attempting a risky
  register-budget restructuring under this run's own time pressure — a
  real candidate for a dedicated future slice, but the two genuine
  register-indexing bugs already found and fixed this run are reason
  enough to want more room to test a similar change properly rather than
  rush it.

- Final re-audit of this whole run: the same full `tests/cases/*.di`
  differential sweep from the start (824 files, well beyond the curated
  `parser_cases`/`parser_error_cases` corpora `make test-all` actually
  runs) now shows 24 mismatches, down from 276 at the start — every one
  of them now individually accounted for, not just counted:
  - **1 confirmed deliberate scope cut**: `deep_closure.di` (the nesting-
    depth restriction from the slice above).
  - **11 confirmed non-bugs**: the format-mismatch artifact from the
    slice above (`enumerable_sort_hash_unsupported.di`,
    `enumerable_sum_hash_unsupported.di`, `raise_stack.di`,
    `stack_trace.di`, `operator_def_outside_class_rejected.di`, the 6
    `keyword_args_error_*` cases) — self-hosted already produces the
    identical error text in every case, just presented differently by
    this run's own ad-hoc diff script than by the corpora that actually
    check it correctly.
  - **5 confirmed test-harness noise**: `legacy_0321.di`–`legacy_0325.di`
    (Fiber tests) differ only in ASan's own startup banner, which embeds
    the current process's PID — two different invocations of the sanitize
    binary necessarily print two different numbers there; nothing to do
    with Diamond at all.
  - **7 confirmed instances of the one remaining real, deliberate scope
    cut**: `legacy_0109.di`, `legacy_0181.di`, `legacy_0190.di`,
    `legacy_0193.di`, `legacy_0194.di`, `legacy_0289.di`,
    `nilable_types.di` — all return a plain `Instance` as their top-level
    result, the one heap-object kind `copy_value_into_vm` still declines
    (see the very first slice of this run): an `Instance`'s `->class`
    pointer aims into the *source* `DiamondProgram`'s own `classes[]`
    array, which has no lifetime guarantee once the caller lets go of the
    `ProgramBuilder` that owns it. Copying the `DiamondValue` itself
    wouldn't make that dangling reference safe — this would need either
    copying the referenced class definition too (a much bigger, separate
    project: classes can reference other classes, interfaces, and their
    own methods' bytecode) or some other way to keep the source program
    alive for as long as the copy lives, neither of which this run
    attempts. Every other heap-object kind this run set out to fix is
    fixed; this one stays open, on purpose, as the honest remaining edge
    of `ProgramBuilder#run`'s result support.

- This note used to read "Self-hosting, Phase 3 sub-phase 5: exceptions,
  modules, and `require`" — stale by the time this run started, not
  after it. `selfhost/parser.di` already had roughly 85 slices of
  exception-handling coverage (`raise`/`rescue`/`ensure`/`retry`, typed
  filters, `rescue else`, dozens of matching error-diagnostic cases) and
  substantial module support (`compile_module`/`compile_module_function`/
  `compile_module_include`, structural `interface`s) well before this
  run touched anything. `require` was never a parser-level gap at all —
  it's resolved by the native `ProgramBuilder#expand_source` bridge
  before the self-hosted `Parser` ever sees a token, exactly mirroring
  how `require` is a loader-level, not parser-level, concern for the
  native compiler too. Verified all of this directly this run via a full
  824-file differential sweep, not by trusting the old note — see the
  slices above for what that sweep actually found instead (a
  `ProgramBuilder#run` result-type gap dwarfing everything else, and,
  once that stopped hiding them, three genuine self-hosted-compiler
  correctness bugs).

- The real Phase 4 bootstrap, attempted and working: `selfhost/
  self_parse_check.di` only ever checked that the self-hosted `Parser`
  successfully *compiles* its own two source files (`parser.compile()`
  returning `true`) — it never went on to actually run the compiled
  result. New `selfhost/self_run_check.di` does: it appends a small
  driver (`target_path = gets(); puts(parse_and_run_with_core(target_
  path))`) to the bundled `parser.di`+`lexer.di`+`core.di` source before
  compiling, so running the self-compiled result performs the exact same
  "compile and run an arbitrary program" operation `parse_and_run_with_
  core` does natively — except every step of it now executes as bytecode
  the self-hosted compiler itself produced, one `DiamondVm` level deeper
  (native VM → self-compiled parser/lexer running in a child VM → *that*
  code's own `ProgramBuilder`/`Parser` compiling and running a third,
  independent target program in a grandchild VM). Tested against a real
  multi-feature program (`tests/parser_cases/generic_collection_
  constraints.di` — generics, exceptions, rescue) with byte-for-byte
  correct output, and against a known-`Instance`-result program
  (`legacy_0109.di`) to confirm the documented gap propagates as a clean,
  correctly-attributed `TypeError` with a full four-frame stack trace
  across all three VM levels, not a crash. Wired into `tests/
  parser_diff.sh` as a permanent "self-hosted parser self-run bootstrap
  check" right after the existing compile-only one, so this doesn't
  silently regress.

- Lifted the self-hosted parser's nested-closure depth restriction
  (`@function_nesting_depth >= 2` in `compile_definition`, rejecting
  `deep_closure.di`). Turned out stale, not load-bearing: `compile_
  function_body` already fully saves and restores every relevant piece of
  compiler state (`@locals`, `@next_register`, `@code_count`, `@type_
  facts`, ...) around each nested function body, and ordinary recursion —
  Diamond's own call frames — handles arbitrary depth correctly with no
  shared-state risk, register budget or otherwise. The restriction
  predates that state-save/restore existing at all (the era the header
  comment's "first, single-method version... exhausted [the budget]
  outright" describes) and was simply never revisited once the split
  happened. Verified 2 and 3 levels of nested closures both compile and
  run correctly, byte-for-byte matching native, including inside a class
  instance method. New `tests/parser_cases/deep_nested_closures.di` locks
  it in.

  Investigating *why* it seemed unsafe surfaced a real, separate, native
  `compiler.c` bug, deliberately left unfixed here rather than rushed: an
  ordinary closure nested inside a class's *instance* method (not a
  `self.`-prefixed singleton method) gets `function->owner_class` and a
  self-register reservation it shouldn't — `compiler->current_class` is a
  compiler-wide "lexically inside a class body" flag, true for a nested
  closure at any depth, not just a direct member, and this code path
  doesn't yet distinguish the two. Confirmed via `--dump-bytecode`: a
  `def add(x)` nested inside an instance method read its own `x` from
  register 1 (assuming a self slot at register 0), while `CALL_CLOSURE`
  (the actual calling convention for an ordinary closure — no receiver)
  always places argument 0 in register 0, off by exactly one both in
  where the value landed and in the resulting arity check. Attempting the
  obvious fix (gate both the self-reservation and the `owner_class`
  assignment on genuine top-level-member status) broke a real, different,
  already-tested feature: `redefine_method`'s "patch factory" idiom
  (`def self.make_patch(); def replacement(); @ivar...; end; replacement;
  end`, see `legacy_0093.di`/`legacy_0094.di`/`legacy_0095.di`)
  *deliberately* relies on a closure nested inside a *singleton* method
  getting a self slot and `@ivar` access — confirmed by reverting the fix
  and watching `Shape.redefine_method("area", ...)` break with "callable
  must be a method of 'Shape'" on code that's supposed to succeed. The
  real fix needs a narrower signal than `at_top_level` — something like
  "was the *immediately enclosing* def itself a class singleton method,"
  not "is this def nested at all" — which is real, careful design work,
  not a mechanical change; reverted rather than shipped partially
  verified.

- Setting up GitLab CI (`.gitlab-ci.yml`, a `fedora:latest` image; `../
  reginold` cloned from its own now-independent private GitLab project
  via a read-only project access token, since `make test-all` needs it
  as a sibling checkout) surfaced a real, previously-unnoticed
  portability bug: `tests/run.sh`'s directory-as-argument case (`diamond
  tests` should fail with `cannot read 'tests': Is a directory`) failed
  silently and consistently on GitLab's actual runner while passing
  every time in a locally-reproduced fresh-clone container, including
  under a deliberately CPU-throttled one — ruling out resource
  constraints as the cause. Root cause, found via a temporary diagnostic
  `bash -x tests/run.sh` CI run: `read_file` (`main.c`) opens the path
  and later expects `fread` to fail with `EISDIR` when it's actually a
  directory — true on this development machine's filesystem, but `read
  (2)` returning `EISDIR` for a directory descriptor is a Linux
  filesystem-driver behavior, not a POSIX guarantee, and evidently
  doesn't hold on whatever backs GitLab's own runner's build directory
  (very plausibly overlayfs, common for container CI). Fixed by checking
  `fstat`+`S_ISDIR` immediately after `fopen` and reporting `EISDIR`
  explicitly, rather than inferring "is a directory" indirectly from how
  a specific filesystem's `read(2)` happens to fail — deterministic
  across filesystems instead of dependent on driver behavior. A real
  correctness fix, not a CI-specific workaround; verified locally against
  both this machine's own filesystem and a fresh-clone `fedora:latest`
  podman reproduction of the CI environment.

  Fixing `main.c`'s `read_file` let CI progress from ~1 minute to over 8,
  reaching all the way to `test-parser-diff` before failing on a second,
  *real* mismatch: `tests/parser_error_cases/require_unreadable_manifest.
  di` (a package whose manifest path, `package.di`, is deliberately a
  directory rather than a file) got a different error on the real runner
  than on this machine. Same bug, different copy: `loader.c`'s own
  `read_source` — used for both package manifests and `require`d file
  bodies — has the identical `fopen`-succeeds/`fread`-assumed-to-fail
  pattern, entirely independent of `main.c`'s copy. Fixed identically
  (`fstat`+`S_ISDIR` right after `fopen`, `errno=EISDIR` set explicitly
  before returning). Two independent instances of the same underlying
  assumption is worth remembering as its own lesson: "let `read(2)` fail
  distinctively" is not a safe idiom for detecting a directory anywhere
  in this codebase, on any filesystem.

  `make test-all` now passes cleanly on GitLab's own runner (5m29s,
  https://gitlab.com/dmn9180/diamond/-/pipelines) — CI genuinely runs the
  full suite end to end, not just a subset, and found two real portability
  bugs neither this development machine nor local `podman` reproductions
  (including a deliberately CPU-throttled one) ever would have.

- Added a REPL (`src/repl.c`, launched by running `diamond` with no
  arguments when stdin is a terminal). The real design problem wasn't
  syntax — it was making later evaluations see earlier ones' locals,
  functions, and classes without replaying earlier `puts` output on every
  later round, given that `diamond_compile`/`diamond_vm_run` have no
  notion of a persistent scope across separate calls; each compile starts
  a fresh program from register 0. True incremental compilation (adding
  functions to an *existing* `DiamondProgram` and resuming a persistent
  `DiamondVm`'s register state) would need real VM surgery beyond this
  slice's scope, so v1 takes a different, much smaller-diff path: the
  whole session's source accumulates verbatim, and *every* evaluation
  recompiles and reruns the full accumulated buffer from scratch — but
  its stdout is captured to a `tmpfile()` (never a real pty/pipe write)
  rather than the terminal, and only the *suffix* beyond what the
  previous round's capture already contained gets shown, since
  `session + old input` is always a byte-identical prefix of `session +
  old input + new input` for anything deterministic. No VM changes, no
  new opcodes — just diffing what a full recompile-and-rerun already
  produces. Real limitations from this choice, on purpose: genuinely
  non-deterministic output (real-world timing, I/O interleaving) can't
  reproduce that prefix exactly, and the code falls back to showing
  everything rather than guessing wrong; redefining a name is rejected
  exactly like a single program would reject it (no special-cased "REPL
  redefinition" allowance) since there's no reliable way to identify and
  excise a prior definition's exact source span without a real parser
  hook this slice didn't build.

  Multi-line input detection reuses the compiler's own diagnostics rather
  than a hand-rolled block-depth scanner: if a compile fails and the
  failing diagnostic's own line lands on-or-past the last line the
  buffer-so-far actually has (e.g. "expected 'end' after if expression"
  at EOF), that's treated as "needs another line," not a real error —
  imperfect (it can't distinguish every possible trailing syntax error
  from truly incomplete input), but it correctly handles every close-a-
  block case tried against it and never hangs on a genuine error.
  `DIAMOND_FORCE_REPL` (checked alongside the normal `isatty` gate) exists
  purely so `tests/repl_test.sh` can drive the REPL over a bash `coproc`'s
  pipes, which are never a real pty.

- Fixed the native `compiler.c` bug found above properly this time, with
  the narrower signal the earlier attempt was missing: a new `Compiler.
  in_singleton_method` field, tracking whether `compiler->function` (the
  function whose body is *currently* being compiled) is itself a class/
  module singleton method (`def self.name`) — saved and restored around
  each nested function body exactly like `in_method`/`current_method`
  already were, and set at the point `compiler->function` switches to a
  new function, from *that* function's own `at_top_level && module_
  singleton`. The self-register-reservation and `owner_class` sites both
  now key off `direct_class_member || direct_module_member ||
  nested_in_singleton_method` — the first two unchanged from before
  (`at_top_level` plus being lexically inside a class/module body), the
  third checking the *enclosing* function's `in_singleton_method` value,
  captured before this def's own switch overwrites it. A closure nested
  immediately inside a singleton method gets the instance-method-like
  treatment `redefine_method`'s patch-factory idiom needs; a closure
  nested anywhere else — an ordinary instance method, a plain top-level
  function, or two-or-more levels deep from a singleton method — gets
  none, matching a plain closure.

  Verified all four `redefine_method` failure-path assertions in `tests/
  run.sh` (mismatched arity, unknown method name, a capturing closure,
  and a callable from a different class) still produce their exact
  original error text, `legacy_0093.di`/`legacy_0094.di`/`legacy_0095.di`
  (the real patch-factory tests) still pass, and the original bug reports
  (`add` nested in an ordinary instance method, and two levels of nesting
  inside one) are fixed — confirmed the "different class" case in
  particular still correctly distinguishes owner classes, not just
  blindly stopped rejecting everything. New `tests/cases/closure_nested_
  in_instance_method.di` locks in the fix: a single-level and a
  two-level case, both nested inside ordinary (non-singleton) instance
  methods.

- Added `String#gsub`, `String#sub`, and `String#scan`, closing the gap
  between `Regexp` (which only had `.match`/`.match?`) and `String` —
  there was previously no way to use a regex to transform or extract
  from a string at all. `gsub`/`sub` share one helper,
  `regexp_replace_helper` (a `replace_all` bool is the only difference
  between "replace every match" and "replace the first match only"),
  built on a small growable `ByteBuffer` that interleaves unmatched
  source spans with the replacement as `reginold_search` walks forward
  from an advancing cursor. `scan` (`regexp_scan_helper`) walks the same
  way but collects an `Array` instead of building a string: a `String`
  per match when the pattern has no capture groups, or an `Array` of
  per-group `String`/`Nil` (unmatched optional group) when it does —
  matching Ruby's own `#scan` split exactly. Both helpers advance the
  cursor by one byte past a zero-length match (e.g. `x*` against text
  with no `x`) instead of re-searching from the same offset, the same
  guard `gsub` needs in Ruby to avoid looping forever on a pattern that
  can match empty.

  v1 shipped without `\1`-style backreference substitution in the
  replacement string (see below for when that landed).

  Verified directly against representative inputs (captureless and
  capturing patterns, a zero-length-matching pattern confirmed not to
  hang, wrong-type pattern/replacement arguments, wrong arity) before
  writing them down as permanent cases: `tests/cases/string_gsub_
  replaces_all`, `string_sub_replaces_first_only`, `string_gsub_zero_
  length_match`, `string_scan_no_groups`, `string_scan_with_groups`,
  `string_gsub_pattern_not_regexp`, `string_gsub_replacement_not_
  string`, `string_scan_pattern_not_regexp`, `string_gsub_wrong_arity`.
  `make test` (880 assertions, up from 871) passes clean.

- Added nine more `String` methods, closing the remaining gaps between
  what Diamond's core prelude offered and what everyday string handling
  needs: `start_with?`, `end_with?`, `include?`, `capitalize`, `chars`,
  `bytes`, `chomp`, `ljust`, `rjust`. All follow the existing dispatch
  table's established shape (fixed arity, explicit type checks on each
  argument, `ArgumentError`/`TypeError` on misuse) — no new machinery,
  just filling in the table. `chars`/`bytes` stay byte-oriented, matching
  every other String primitive in this VM (`ord`, `slice`, `split` with
  an empty separator, `reverse`) — none of them are Unicode-codepoint-
  aware, so giving `chars` codepoint awareness while everything else
  stays byte-oriented would be a worse inconsistency than the byte-level
  behavior itself. `ljust`/`rjust` require the padding string as an
  explicit second argument (no Ruby-style default `" "`) since native
  dispatch here has no optional-argument convention to hook into; an
  empty padding string is rejected as `ArgumentError` (matching Ruby)
  rather than silently no-op'd, to avoid a padding string that can't
  actually pad becoming a silent truncation-to-original-length bug.

  Deliberate scope cut, left for a future round if something needs it:
  `String#tr` (character-set translation with range/negation syntax like
  `"a-z"` or `"^abc"`) — a bigger parsing job than the other eight
  combined, and nothing in this codebase currently exercises it.

  Verified directly (prefix/suffix/substring matching including the
  empty-needle edge case, capitalize on empty/all-caps input, chomp's
  `\r\n`/`\n`/`\r`-alone cases, ljust/rjust with both single- and multi-
  character padding, negative width and empty-padding error paths) before
  writing eleven permanent cases: `tests/cases/string_start_with`,
  `string_end_with`, `string_include`, `string_capitalize`,
  `string_chars`, `string_bytes`, `string_chomp`, `string_ljust_rjust`,
  `string_start_with_argument_not_string`, `string_ljust_empty_padding`,
  `string_ljust_negative_width`. `make test` (891 assertions, up from
  880) passes clean.

- Added `Int#chr` (`src/vm.c`) — the inverse of `String#ord`, a single
  byte (0-255, `RangeError` outside that) as a one-character `String`.
  Structurally it needed its own small carve-out: `INVOKE`'s dispatch
  gate rejected every non-`DIAMOND_VALUE_OBJECT` receiver outright, since
  Int/Float/Bool have no method-call support of any kind in this VM (only
  free functions like `abs`/`min`/`max` in `lib/core.di`) — extending
  that gate for one narrowly-scoped method (an early branch, checked
  before the existing object-only gate, not a rewrite of it) was less
  invasive than the alternative of inventing a whole new native-global-
  function mechanism to match `puts`. Byte-level by design, matching
  every other String/Int primitive already in this VM.

- Added a `JSON` module (`lib/core.di`) — `JSON.stringify`/`JSON.parse`
  — closing the gap `docs/lsp.md`'s own LSP had already run into and
  hand-rolled around in C (`lsp/*.c` has its own JSON reader/writer
  specifically because "Diamond has no JSON support," a line that was
  sitting in this file's own LSP entry above). Implemented as ordinary
  self-hosted Diamond, not a VM primitive — a JSON codec is pure logic
  over values the language can already build (`Hash`, `Array`, `String`,
  `Int`, `Float`, `Bool`, `Nil`), same reasoning as the Enumerable
  methods earlier in this file.

  The real design obstacle wasn't JSON grammar, it was the host
  language: a recursive-descent parser is inherently mutually recursive
  (`parse_value` calls `parse_array`/`parse_object`, which call
  `parse_value` back), and this compiler resolves a bare no-receiver
  call (`foo(x)`) against locals and already-declared top-level
  functions *at compile time, in file order* — confirmed by testing two
  plain top-level functions calling each other and, more surprisingly,
  two methods of the same class calling each other by bare name from
  inside method bodies (`is_odd(n - 1)` from within `is_even`) — both
  fail with "undefined function" the same way, meaning it's not merely a
  top-level-vs-method distinction. What does work, confirmed by the same
  test with one change: qualifying the sibling call as `self.is_odd(...)`.
  That path goes through `INVOKE`, resolved by name against the callee's
  actual class *at call time*, not by direct reference at compile time —
  so declaration order stops mattering. `JSONCodec` bundles every parse/
  stringify step as a method of one class for exactly this reason, and
  every internal call between them is `self.`-qualified; `JSON.parse`/
  `JSON.stringify` are thin `module_function` wrappers that each spin up
  a `JSONCodec.new()` and delegate. (This ordering rule is a real,
  reproducible compiler behavior, not a bug filed here — self-recursion
  already worked without this trick, since a top-level function's own
  name is registered before its body compiles; only *sibling* forward
  references needed the `self.` workaround. Worth remembering next time
  anything mutually recursive gets written in Diamond itself.)

  `\uXXXX` escapes decode BMP codepoints (0x0000-0xFFFF, the full range
  four hex digits can express) and get properly UTF-8-encoded via plain
  integer division/modulo (this VM has no bitwise operators) into 1-3
  raw bytes — consistent with every other String primitive here staying
  byte-oriented rather than codepoint-aware. Surrogate pairs (astral
  characters split across two `\uXXXX` escapes) are a deliberate scope
  cut: `JSONError` is raised on a lone surrogate rather than silently
  emitting invalid UTF-8. `Int#chr` (added alongside this, see above)
  is what makes the byte-level encoding possible at all — without it
  there was no way to build a String from an arbitrary control-range or
  non-ASCII byte value from inside Diamond source. Hash keys that aren't
  already Strings get stringified through the same `"#{key}"` conversion
  `array_join` already relies on, matching how Ruby's own `JSON.generate`
  handles non-String Hash keys.

  Verified directly (nested objects/arrays, all six escape sequences
  Ruby also special-cases plus `\uXXXX` including a 2-byte UTF-8 case,
  Bignum values via existing auto-promoting `to_i`, malformed input at
  each grammar production, whitespace tolerance, trailing-content
  rejection) before writing fourteen permanent cases (encode, decode,
  round-trip, and error paths): `tests/cases/json_stringify_scalars`,
  `json_stringify_collections`, `json_stringify_escapes`,
  `json_stringify_control_char`, `json_parse_scalars`,
  `json_parse_collections`, `json_parse_escapes`, `json_round_trip`,
  `json_parse_malformed_object`, `json_parse_unterminated_array`,
  `json_parse_trailing_content`, `int_chr`, `int_chr_out_of_range`,
  `int_undefined_method`. Note: `Hash`/`Array` equality (`==`) in this
  VM is identity-based, not structural (a pre-existing, unrelated VM
  characteristic, not something this work changed) — `json_round_trip`
  compares individual decoded leaf values rather than the whole
  structure for that reason. `make test` (905 assertions, up from 891)
  passes clean.

- Added `String#tr` (`src/vm.c`) — the scope cut left over from the
  String batch above, picked back up now that it's the only String
  method still missing. Ruby's `tr(from, to)`: expand both spec strings
  (backslash-escaping the next byte, `c1-c2` ranges, and a leading `^`
  on the from-spec only, meaning "match everything *not* in this set")
  into flat byte lists with a new helper, `tr_expand_spec`; build a
  256-entry replacement table from the from/to byte lists (`to`
  positionally, padded by repeating its last byte once it runs out; an
  empty `to` means delete rather than replace); then walk the source
  string applying it. Byte-oriented like every other String primitive
  here — `a-z` is a *byte* range, not a Unicode one, consistent with
  `ord`/`chr`/`chars`/`bytes` already being byte-level. An empty
  from-string is rejected as `ArgumentError` (mirrors Ruby, and the
  `ljust`/`rjust` empty-padding precedent above) since it can't express
  any substitution rule at all.

  The one open design call was what a negated from-spec (`^`) should
  map *to*, since there's no positional correspondence between "every
  byte not in a handful of listed bytes" and `to`'s byte list the way
  there is for the plain case — resolved the same way Ruby does: every
  non-member byte maps to `to`'s single last byte (or gets deleted, if
  `to` is empty). Confirmed against Ruby's own documented `tr` examples
  directly (`"hello".tr("el", "ip")` → `"hippo"`, `"hello".tr("^aeiou",
  "*")` → `"*e**o"`) before writing `tests/cases/string_tr` (the
  substitution/negation/deletion/range/escape cases above, all in one
  array literal), `string_tr_empty_from`, and
  `string_tr_argument_not_string`. `make test` (908 assertions, up from
  905) passes clean.

- Added `\1`-style backreference substitution to `String#gsub`/`#sub`
  replacement strings (`src/vm.c`), closing the v1 scope cut noted
  above. New helper `regexp_append_replacement` expands the replacement
  byte-by-byte instead of copying it verbatim: `\0`/`\&` is the whole
  match, `\1`-`\9` is that numbered capture group (empty string if the
  group didn't participate, e.g. an unmatched `(x)?`, or if the pattern
  doesn't have that many groups — no error either way, matching Ruby's
  own quiet-empty-string behavior rather than raising), `\\` is a
  literal backslash, and a stray backslash before anything else just
  drops the backslash and keeps going. `regexp_replace_helper` now
  keeps each `reginold_match` (captures included) alive through the
  replacement-expansion call instead of freeing it right after reading
  `overall`. Named backreferences (`\k<name>`) are a further scope cut
  — reginold's own capture API is index-only, nothing here has a
  reachable named-group source to test against anyway.

  Verified directly (multi-group reordering on a date pattern,
  whole-match doubling via `\0`, an unmatched optional group producing
  an empty splice, a literal `\\` followed by a plain `n` producing a
  two-byte `\n` rather than a newline, and a `\9` past the pattern's
  actual group count producing nothing) before writing four permanent
  cases: `tests/cases/string_gsub_backreferences`,
  `string_gsub_whole_match_backreference`,
  `string_gsub_unmatched_group_backreference`,
  `string_gsub_literal_backslash_and_out_of_range_group`. `make test`
  (912 assertions, up from 908) passes clean.

- Gave `editors/vscode/` a real LSP client (`extension.js`), closing the
  gap between "the language server exists" and "VS Code actually shows
  its diagnostics": the extension previously did syntax highlighting
  only, with no code that ever spawned `diamond-lsp` at all. Hand-rolled
  rather than built on `vscode-languageclient`, the same "from scratch,
  zero external dependencies" choice `lsp/` itself already made (see
  `docs/lsp.md`) — `diamond-lsp` only speaks a small, fixed protocol
  subset (`initialize`, `initialized`, `didOpen`/`didChange`/`didClose`,
  `shutdown`/`exit`, one diagnostic per publish), so a full client
  library's generality buys nothing here. Plain CommonJS requiring only
  `vscode` and `child_process` — no `node_modules`, no npm install, no
  build step, on either side: VS Code's own extension host already
  bundles Node. New `diamond.languageServerPath` setting (default
  `diamond-lsp`, i.e. PATH lookup) and a `Diamond: Restart Language
  Server` command for after rebuilding the binary.

  Verification used `node` (available in this environment via `mise`,
  not previously known to be — see the syntax-highlighting entry above,
  written when it looked absent) to drive the real `extension.js`
  against the real `build/diamond-lsp` binary, with a small hand-written
  stub `vscode` module (`Range`/`Diagnostic`/`Uri`/`workspace`/
  `languages`/`commands`, just enough surface for this client) standing
  in for the actual VS Code API — not a mock of the extension's own
  logic, the real file, spawning the real server process over real
  stdio pipes. This caught two genuine bugs no amount of reading would
  have: (1) the client's own `didClose` handler called
  `diagnosticCollection.delete()` immediately, but the server *also*
  publishes an empty `publishDiagnostics` for that uri on close (its own
  documented way to clear a closed file, per `lsp/main.c`) — that publish
  arrives asynchronously and lands after the delete, silently re-adding
  an (empty, but present) entry; fixed by dropping the redundant
  client-side delete and letting the server's own publish be the only
  source of truth. (2) A launch failure (bad/missing
  `languageServerPath`) only ever emits Node's `'error'` event on at
  least this platform, never a following `'exit'` — confirmed directly
  against plain `child_process.spawn` with a nonexistent path, not
  assumed from the docs — so `child` was only ever cleared in the
  `'exit'` handler, meaning every subsequent `stopServer()` (on
  deactivate or restart) still saw a "live" child, sent it a `shutdown`
  request nothing would ever answer, and hung forever awaiting a
  response that could never arrive; fixed by clearing `child` in the
  `'error'` handler too, plus a 1-second timeout race around the
  `shutdown` request itself as a second line of defense against any
  other way a request could go unanswered. The full open→diagnose→
  fix→clear→close→shutdown lifecycle now passes end-to-end against the
  real binary.

  This Node-driven check isn't a committed regression test: GitLab CI's
  `fedora:latest` image has no `node` (confirmed against
  `.gitlab-ci.yml`), and every other test script in this repo is
  deliberately Python/Node-free (see `tests/lsp_test.sh`'s own header
  comment) so as not to need a second toolchain in CI. Adding one just
  for this would either bit-rot unrun or require changing what CI
  installs — a bigger, separate call this slice didn't make unilaterally.
  The verification above was real and thorough, just not wired into
  `make test-all`.

- Added `textDocument/hover` to `diamond-lsp` (`lsp/hover.c`, new) and
  wired it into the VS Code client (`editors/vscode/extension.js`) —
  the first `lsp/` capability beyond diagnostics, and the first real
  test of `docs/lsp.md`'s own claim that hover/go-to-definition/
  completion "all need a real symbol table." They do, in general — but
  hover specifically doesn't have to wait for one: this language
  resolves a bare call to exactly one top-level function *at compile
  time* (no overloading), and a class name always names exactly one
  class, so both are already unambiguous without any scope resolution
  at all. Scoped to exactly those two identifier kinds for exactly that
  reason; a method reached through `receiver.method(...)` is the case
  that still needs real type inference (which class's method depends on
  `receiver`'s runtime type) and stays out of scope.

  Mechanically: tokenizes the *raw* open-document text directly (via
  `src/lexer.c`, independent of compilation) to find the identifier
  under the cursor, then does a plain name lookup against the
  *compiled* program's function/class tables (built the same prelude+
  require-bundling way `diagnostics_compute` already does, deliberately
  re-derived rather than shared — see the comment in `hover.c` for why
  not sharing was the safer call given `diagnostics_compute` is already
  tested and this was a second, independent consumer). No position
  translation between the two is needed at all, unlike diagnostics:
  hover never has to map a position in the bundled compile buffer back
  to the editor's own document, since the cursor position is resolved
  against the raw text *before* compiling, and the compiled program is
  only ever consulted by name afterward. Real signature reconstruction
  reuses `disassemble.c`'s own type-set formatting (`print_type_set`,
  now also exposed as `diamond_print_type_set`) rather than duplicating
  it — which is also how a real, non-obvious bug surfaced and got fixed
  before shipping: a function's `parameter_type_sets`/`return_type_set`
  index into *that function's own* `type_sets[]` array, not the
  chunk-wide one `diamond_program_chunk` returns (which is actually just
  the entry/top-level function's table) — `diamond_disassemble` already
  knew this (it builds a per-function `DiamondChunk` with only
  `type_sets` swapped in before formatting), and `hover.c`'s first draft
  didn't, silently printing the *entry* function's unrelated types
  instead (caught immediately by the Node-driven end-to-end check below,
  not by inspection).

  Verified two ways: `tests/lsp_test.sh` gained real hover cases (a
  function's own declaration, a bare call site, a class with and
  without a superclass, a local variable correctly returning nothing, a
  document that doesn't currently compile correctly returning nothing)
  — and its pre-existing "unrecognized method" case, which had been
  using `textDocument/hover` as a conveniently-realistic-but-then-
  unimplemented example, switched to `textDocument/definition` (still
  genuinely unimplemented). Also re-ran the Node-driven `extension.js`
  check from the entry above, extended with a stub
  `registerHoverProvider`/`Hover` on the fake `vscode` module, confirmed
  against the real server end-to-end (not wired into `make test-all`,
  same reasoning as before). `make test`/`make test-lsp` (912 / 30
  assertions, up from 912 / 22) pass clean.

- Added `textDocument/definition` and `textDocument/documentSymbol` to
  `diamond-lsp` (`lsp/definition.c`, `lsp/document_symbol.c`, both new),
  wired into the VS Code client alongside hover. Same two identifier
  kinds hover resolves (top-level function, class — both globally
  unambiguous by construction, see hover's own entry above), just two
  more things to do with a match instead of a signature string: jump to
  it, or list every one of them.

  This needed a real declaration-site position, which nothing persisted
  past compilation before now — `src/compiler.c`'s `compile_definition`/
  `compile_class` had the real `DiamondSpan` (with line/column *and* the
  byte offset the lexer was tracking) sitting right there at the point
  of declaration, used for `fail()` diagnostics and then discarded. Now
  copied into three new fields each on `DiamondFunction`/`DiamondClass`
  (`declaration_line`, `declaration_column`, `declaration_start` —
  `src/vm.h`) for every function and class, top-level or not (cheap to
  populate uniformly even though only top-level ones are ever looked up
  by `lsp/` today).

  The interesting part turned out to be position *resolution*, not
  position *storage*. A first instinct — "a declaration actually in the
  open document's own text needs no translation, only a `require`d
  file's does" — is wrong: `diamond_load_program` (`src/loader.c`)
  inserts a `#line 1` reset before *every* contiguous chunk it copies
  into the compiled buffer, including the requesting document's own
  content around a `require` line, not just before an inlined
  dependency's. Since the lexer's `#line 1` handling (`src/lexer.c`) is
  a hardcoded reset-to-exactly-1, not a general `#line N` parser, a raw
  `declaration_line` is only ever correct for the first thing after the
  *most recent* reset — anything declared later in the same file, after
  a `require`, needs the identical segment-relative remap
  (`mapped_original_line` + counting newlines since the segment start) a
  `require`d file's own declaration needs. `definition.c` got this right
  from the start by running every match through
  `diamond_resolve_diagnostic_location` (`src/compiler.h`) unconditionally
  — the same function a compile error's own position already goes
  through — and comparing its resolved `.path` against the requesting
  document's own to decide whether to reuse the request's `uri` verbatim
  or synthesize one for a different file (new `diagnostics_path_to_uri`,
  the encode-side inverse of the existing `diagnostics_uri_to_path`,
  `lsp/diagnostics.c`).

  `document_symbol.c`'s first draft didn't reuse that function — it
  reimplemented "is this declaration in a `require`d file" as "does its
  offset fall inside any of `bundle`'s segments," which is simply wrong:
  segments cover *every* chunk the loader copies in, including the
  requesting document's own (each one still carries a real `path`, just
  the same path as the document itself) — so the first version excluded
  every declaration in the file, always, full stop (caught immediately
  by manual testing, not left for the test suite to find). Fixing that
  down to "compare the segment's path" surfaced the deeper raw-line bug
  above on the very next test (a document with a `require` followed by
  its own `def`, whose reported line was off by exactly the required
  file's own line count) — rewritten to call
  `diamond_resolve_diagnostic_location` per symbol, same as
  `definition.c`, which fixed both at once. `definition.c` itself never
  had either bug, confirmed by adding the same "declare something after
  a `require`, in the same file" case to its own tests once the pattern
  was known to be worth checking.

  Document symbols deliberately list only what's declared *in the open
  document itself* — not lib/core.di's prelude (excluded the same way
  `definition.c` excludes it, `declaration_start>=user_offset`) and not
  anything pulled in via `require` (its own file has its own outline, a
  `didOpen` away) — an outline showing symbols that aren't actually in
  the file would be more confusing than useful.

  Verified directly (declaration and call-site go-to-definition,
  self-round-trip on a class declaration, cross-file resolution into a
  required file, local variables and non-compiling documents correctly
  returning nothing for all three, document symbols on a document with
  a `require` *and* its own trailing declarations, an empty-outline
  document distinguished from a non-compiling one) before extending
  `tests/lsp_test.sh` and the Node-driven `extension.js` end-to-end
  check from the entries above (new `Location`/`DocumentSymbol` stubs on
  the fake `vscode` module; same "not wired into `make test-all`"
  reasoning). `make test-lsp` (46 assertions, up from 30) passes clean;
  `make test` stays at 912 (nothing here is observable from Diamond
  source itself).

## Later experiments

- Self-hosting the compiler and core libraries in Diamond (in progress —
  see `Completed foundation` for Phase 0-2, Phase 3 sub-phases 2-5 (all
  landed, sub-phase 5 far deeper than its own name suggests — see its
  many dozens of slices), and the real Phase 4 bootstrap demonstration
  just above: the self-compiled parser successfully compiling and running
  a third, independent program. What remains multi-session future work:
  the native `compiler.c` closure-inside-a-method bug (`Next priorities`),
  any narrower compiler.c feature-parity gaps a future differential sweep
  might still turn up, and `Instance` results crossing a
  `ProgramBuilder#run` boundary, the one documented gap this run's own
  bootstrap test hit directly).
- Native-code generation or a tracing/method JIT — nothing in the
  `jit-experimentation` work above generates native code; it's all
  interpreter-loop leaning (register zero-init, opcode dispatch,
  struct-copy elimination). Actual JIT compilation remains a distinct,
  larger, not-yet-attempted piece of work.
- Test suite is getting slow to run locally, and it's a real cost, not a
  hypothetical one: `tests/run.sh` spawns a brand-new `./build/diamond`
  process per case (866 `.di` files under `tests/cases/`, 912 assertions
  total as of `String#tr`/gsub-backreferences), and every single one of
  those processes recompiles all 940 lines of `lib/core.di`'s prelude
  from scratch before it ever reaches the case's own few lines — there's
  no persistent process, no cached/precompiled prelude, nothing shared
  across cases at all. Measured directly: a full `make test` run takes
  ~60s wall clock, and the time split (`10s user` / `46s sys`) says most
  of that is fork/exec/process-startup overhead, not actual compiler or
  VM CPU work — which matters for *what* fix would help: micro-
  optimizing `diamond_compile` wouldn't touch this, since the cost is in
  spinning up ~900 fresh OS processes each redoing the same prelude
  compile, not in any one of those compiles being slow. A test runner
  that reuses one process (or at least one pre-compiled prelude) across
  many cases — `lsp/`'s own `diagnostics_compute` already demonstrates
  the shape of this: a lazily-allocated, reused-across-calls
  `DiamondProgram` scratch buffer instead of a fresh one per compile —
  is the likely fix, but changes what "run one `.di` file and diff its
  output" (`tests/run.sh`'s entire current model) means, so it's a real
  design task, not a quick patch.
- Diamond itself has no testing framework — nothing self-hosted for
  someone writing Diamond *programs* (as opposed to this repo's own
  bash-driven `tests/cases/*.di` + `.expected` convention, which tests
  the language from the outside and was never meant to be something
  Diamond code written *in* Diamond could use) to assert against, group
  into suites, or run selectively. A minitest/RSpec-style library —
  `assert_equal`, test classes or blocks, pass/fail/error counts, a
  runnable entry point — is real, user-facing language-completeness
  work, not just internal tooling, and self-hosting it in Diamond itself
  (once the language can comfortably express it) would double as
  another differential-testing surface the way `selfhost/` already is.
  Worth sequencing against the test-runner slowness above: a fast
  in-process runner and a Diamond-native assertion library solve
  adjacent but different problems (this repo's own test speed vs. what
  Diamond programs can use to test themselves) and shouldn't be
  conflated into one piece of work.

## Explicitly deferred

- Ruby compatibility (not a goal; only familiar syntax and object conventions).
- Stable bytecode and embedding APIs.
- Multi-platform support.
- Parallel execution.
