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

## Next priorities

- Self-hosting, Phase 3 sub-phase 5: exceptions, modules, and `require`.
  Phase 4 bootstrap validation remains after the final parser surface is
  ported.

## Later experiments

- Self-hosting the compiler and core libraries in Diamond (in progress —
  see `Completed foundation` for Phase 0-2, Phase 3 sub-phases 2-3 (both
  fully landed), and sub-phase 4's first slice (scalar gradual typing),
  and `Next priorities` for the rest of sub-phase 4; the rest of the
  compiler port and bootstrap validation remain multi-session future
  work beyond that).
- Native-code generation or a tracing/method JIT — nothing in the
  `jit-experimentation` work above generates native code; it's all
  interpreter-loop leaning (register zero-init, opcode dispatch,
  struct-copy elimination). Actual JIT compilation remains a distinct,
  larger, not-yet-attempted piece of work.

## Explicitly deferred

- Ruby compatibility (not a goal; only familiar syntax and object conventions).
- Stable bytecode and embedding APIs.
- Multi-platform support.
- Parallel execution.
