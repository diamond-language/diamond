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

- Integers with checked arithmetic, booleans, `nil`, and strings.
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
- `Int`, `String`, `Bool`, `Nil`, `Array`, `Hash`, and nominal class types.
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

## Next priorities

None queued.

## Later experiments

- JIT compilation for stable specialized bytecode paths.
- Self-hosting the compiler and core libraries in Diamond.
- Structural interfaces and more capable flow typing.
- Native-code generation or a tracing/method JIT.
- Self-hosting selected compiler and standard-library components.
- Dependency resolution, a lockfile, and a way to fetch/install packages
  into a project — the remainder of the "Bundler-like package manager"
  idea beyond bare name resolution and manifests (see `docs/packages.md`
  for what's already built). The manifest's `version` field is already
  declared and type-validated but not yet consumed by anything — this is
  where it would first get read for something. Fetching specifically has
  no buildable target yet: there is no HTTP client and no registry to
  fetch from.

## Explicitly deferred

- Ruby compatibility (not a goal; only familiar syntax and object conventions).
- Stable bytecode and embedding APIs.
- Multi-platform support.
- Parallel execution.
