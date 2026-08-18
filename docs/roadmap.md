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
- Multi-value destructuring assignment (`a, b = expr`): unpacks a single
  `Array`-valued expression across local/`@ivar`/`@@cvar` targets, with
  strict Array-type and exact-length checks (`ArgumentError`/`TypeError`
  on mismatch) rather than Ruby's own lenient nil-pad/truncate behavior.
  See `docs/syntax.md`'s "Multiple assignment".
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
- Flow-sensitive generic result facts for array and hash indexing.
- Growable arrays with guarded `push`/`pop` and Diamond iteration helpers.
- Branch-sensitive nil narrowing with conservative local-fact joins.
- Ordered hash iteration primitives with Diamond-written transforms.
- Non-throwing `is` predicates with primitive and nominal union narrowing,
  including narrowing composed across both operands of `&&`/`||`.
- Runtime `Callable[n]` contracts for Diamond core callbacks, and declared
  `Callable[n, Return]` result contracts with typed array transforms.
- Structural interfaces: a built-in `Sized` interface, user-declared
  interfaces with implicit name-and-arity conformance for native and user
  objects, contravariant-parameter/covariant-return signature checks,
  interface inheritance (`interface Sub < Base1, Base2`), and structural
  matching against the VM's full native method surface (not a fixed
  whitelist).
- Endless expression-bodied function and method definitions.
- Trailing default arguments for functions, methods, constructors, and
  closures.
- Double-quoted expression interpolation with scalar and instance conversion.
- Compile-time multi-file `require` with relative paths, extension inference,
  load-once semantics, and cycle detection.
- Inherited `to_s` stringification for instances and cycle-safe recursive
  collection interpolation.
- Scoped generic function type-variable declarations with runtime erasure,
  runtime generic binding from values and callable returns (including
  recursive Array/Hash binding graphs), and explicit generic call arguments.
- Typed `Callable` parameter signatures with contravariant inputs, covariant
  returns, and generic inference.
- Generic type variables usable as `rescue` filters, resolved against the
  call site's own generic binding.

### Modules, metaprogramming, and dispatch

- Reusable modules with class inclusion, receiver-aware generic methods,
  deterministic precedence, transitive module-to-module inclusion, and
  inheritance integration.
- Lexical module namespaces with nested modules/classes, `Outer::Name`
  resolution, qualified type annotations, and write-once namespace constants.
- Module singleton functions and class singleton methods, including generics,
  defaults, overrides, and superclass-chain lookup.
- Private instance methods (classes and modules), restricted to explicit
  `self` calls, with public/private toggling and named visibility changes.
- Generated `attr_reader`/`attr_writer`/`attr_accessor`/`attr_predicate`/`attr`
  methods, with optional type contracts, comma-separated and parenthesized
  multi-name forms, and targeted visibility changes.
- `module_function` exports (targeted, list, and standalone modes), rejecting
  stateful exports at compile time.
- `alias_method` for class/module instance and writer methods.
- Predicate (`?`) and bang (`!`) suffixed function/method names, integrated
  with visibility, aliasing, and exports.
- Operator overloading: a class can define `+`/`-`/`*`/`/`/`==`/`<`/`<=`/`>`/
  `>=` as ordinary instance methods (unary minus is a `negate` method).
- Keyword arguments (`f(x: 1, y: 2)`) for direct calls to top-level functions.
- Runtime method redefinition (`ClassName.redefine_method(name, callable)`),
  repointing an existing method to an already-compiled, non-capturing
  function of matching arity and owner class.
- Fixed a native closure-capture bug: a closure nested directly inside an
  ordinary instance method (not a singleton method) mis-reserved a `self`
  register it shouldn't have, corrupting argument placement.
- Fixed a second native closure-capture bug: a nested `def` lexically
  inside one branch of an `if`/`else` marks the outer local(s) it captures
  as `captured` for the rest of the enclosing function's compilation --
  correct for the closure-creation site itself (which re-emits
  `BOX_LOCAL` defensively), but every *other* compile-time read/write/call
  of that local (a plain reference, a call through it, an indexed-assignment
  receiver) trusted the flag outright and jumped straight to `GET_CELL`/
  `SET_CELL`, with no guarantee the boxing instruction had actually run on
  whatever control-flow path reached them -- reading it via a sibling
  branch that never took the `if` corrupted the register into
  `DIAMOND_VM_INVALID_BYTECODE` at runtime. Found while writing
  `packages/arel/arel.di`'s `where` method. Fixed by giving the other
  four sites that read or write a captured local (`parse_identifier`,
  `parse_call`'s closure-call path, `compile_index_assignment`'s
  receiver, and `compile_assignment_store`'s captured-local write) the
  same defensive `BOX_LOCAL` re-emission the
  closure-creation site already had -- safe on every path since
  `BOX_LOCAL` is a runtime no-op once the register already holds a Cell.
- Top-level `def`s are now first-class values: a bare top-level function
  name, used without calling it (`f = add`, `apply(add, 1, 2)`), compiles
  to a zero-capture closure value instead of failing with "undefined local
  variable" — the same closure representation a nested `def` already got,
  just for a function that needs no captures. Two pre-existing tests
  (`Fiber`/`File` shadowed by a same-named top-level function) needed
  updating: the shadowing itself still works, but the failure now happens
  a step later and at runtime (`Fiber`/`File` resolve fine as values;
  `.new(...)`/`.open(...)` on a Closure is the actual type error) rather
  than at compile time.
- `@field(...)` now calls a `Callable` value stored directly in an
  instance variable — previously only a local variable holding one could
  be called this way (`@cb()` failed to parse at all, "expected newline
  after expression"), forcing an extra `cb = @cb; cb()` binding step for
  an otherwise ordinary stored-callback-field pattern. The argument-
  parsing-and-`CALL_CLOSURE`-emission logic is now a shared helper used
  by both the local-variable call site and this new one, rather than a
  second hand-copy.

### Control flow, exceptions, and the module loader

- `unless` expressions with `else` and narrowing; `until` loops; loop-body
  `redo`; value-bearing `break` for `while`/`until`/`loop`; unconditional
  `loop do`; chained `elsif`; optional `then`/`do` delimiters; `not`, `and`,
  `or` keyword operators.
- `begin`/`rescue` enhancements: normal-completion `else`, bare re-raise,
  rescue-local `retry`, ordered multiple typed rescue clauses with duplicate/
  unreachable-clause diagnostics, and nominal subclass matching.
- Typed payload fields and causal chaining on user-defined exceptions, with
  runtime-enforced payload contracts and message/cause support across the
  built-in exception hierarchy.
- Postfix `if`/`unless` modifiers on statements and on `break`/`next`/`redo`/
  `return`/`raise`/`retry`, with correct nested-loop/rescue targeting.
- Fixed newline-handling gaps in every bracket-delimited list (array/hash
  literals, call-argument lists, generic type lists, `interface Base <`
  lists) that previously failed to parse across a line break.
- Fixed a general binary-expression newline gap: any expression split across
  lines right after a binary operator (`x = 1 +\n  2`, `if a &&\n  b`,
  comparisons, `is`, ...) previously failed to parse — newline-skipping had
  only ever existed at bracket-list boundaries, never inside `parse_precedence`
  itself. A newline appearing *before* an operator still correctly starts a
  new statement.
- Fixed `x = if ... end` (an `if`-expression as an assignment's right-hand
  side on the `=` line) being misread as a misplaced postfix modifier.
- Compile-time multi-file `require`: CRLF-safe scanning, nested-import source
  mapping, cycle/depth/file-count/segment-size diagnostics, and source-mapped
  runtime stack traces across required files.
- Sequential debug/release/sanitizer build validation wired into the test
  suite.
- Fixed a `raise`-only method/function body failing its own declared
  return-type annotation (`def self.skip(...) -> Bool; raise reason; end`
  used to error with `expression cannot satisfy type annotation`, since
  `compile_raise`'s result register never actually needs to match a
  declared return type — it never returns normally). `compile_sequence`
  now tracks whether its own last top-level statement was an
  unconditional `raise` (`Compiler.sequence_diverges`), and the
  function/method body's final return-type check is skipped when it was.
  A postfix-conditional `raise ... if cond` is deliberately excluded from
  this (the check still applies, since that path might not actually
  raise).
- `Exception#backtrace`, one of the release-readiness gaps a "what do most
  languages have that Diamond doesn't" pass turned up. Returns an Array of
  `"chunk:line:column"` Strings for every still-live call frame, captured
  at `raise` time (not lazily from `#backtrace` itself, since by the time
  a `rescue` clause reads it the deeper frames that were live at the raise
  site are long gone from the VM's frame chain). `DiamondFrame` gained a
  `chunk`/`instruction_offset` pointer pair for this — previously frames
  tracked only registers/pending-unwind state, with no way for an
  ancestor frame to be inspected from below. While building this, found
  and fixed a real bug it happened to expose: a user-defined Exception
  subclass overriding `initialize` to take its own extra arguments could
  never call `super(message)` to reach the built-in constructor —
  `DIAMOND_OP_SUPER` always failed with a spurious `TypeError`, because
  the built-in `Exception#initialize` isn't a real compiled function (it's
  synthesized inline inside `NEW`'s own opcode handler) and `SUPER`'s
  method lookup had no fallback for that case. Custom exception classes
  with extra fields are a completely ordinary pattern, so this was
  blocking real code, not just an edge case.

### Performance: quickening and dispatch caching

- Opcode execution profiling and opt-in dynamic quickening for integer
  addition, subtraction, multiplication, division, ordering comparisons, and
  equality, with safe deoptimization back to the generic opcode and
  configurable warm-up thresholds.
- VM-owned polymorphic inline caches for method call sites, plus a
  monomorphic `INVOKE_MONO` fast path with deoptimization when a new
  receiver class appears, and per-site dispatch telemetry/benchmarks.
- Explicit method-cache invalidation API for class/module mutation
  (redefined methods, changed superclass links, arity/visibility changes),
  shared correctly across independent VM instances.
- Interpreter call-overhead reduction: narrowed per-call register zero-init
  to each function's real high-water mark, elided now-redundant `NIL`
  emissions, and removed a redundant per-call chunk-struct copy.
- Recalibrated `DIAMOND_MAX_CALL_DEPTH` (256 → 100) after discovering real
  recursion could exhaust the C stack well before the old limit tripped,
  especially under AddressSanitizer; added regression coverage for integer
  overflow, dynamic-dispatch arity checking, and a mixed-class dispatch
  benchmark that were previously untested.
- Recalibrated `DIAMOND_MAX_CALL_DEPTH` again (100 → 95) when adding
  native `Time` support: only ~240 bytes of measured `run_chunk`
  frame growth from the new opcodes and arithmetic/comparison fallback
  branches was enough to flip `depth(5000)` (`tests/run.sh`) from a
  clean guard trip into a real ASan stack-overflow — the same redzone-
  per-named-local amplification the original recalibration's own
  comment (`src/vm.c`) already documents, not a raw-byte-count story.
  95 was chosen empirically (every value from 91 through 99 passed
  cleanly; only 100 didn't) to sit in the middle of that window, with
  margin both above `legacy_0091.di`'s `depth(90)` (a hard floor) and
  below wherever the next addition's own growth lands.
- Unified `ADD_INT`'s deopt path with `ADD`'s own fallback logic behind one
  shared helper, fixing a real bug the duplication had hidden: a `+` site
  quickened to `ADD_INT` from prior Int+Int calls raised a spurious
  TypeError on a later Int+Float call instead of promoting, since the
  duplicated deopt copy had never had the mixed-Int/Float case. Also fixed
  three existing `operator_deopt_gap_*` regression tests that were silently
  not exercising the deopt path they were named for (missing the `.env`
  file needed to opt into quickening at all).
- Investigated and closed the "polymorphic inline-cache tier" item this
  file used to list under "Judgement calls" as unimplemented. It isn't:
  `DiamondMethodCache` (`src/vm.h`) is already 4 entries wide
  (`DIAMOND_INLINE_CACHE_WIDTH`), and `lookup_method_cached` already
  linear-probes all of them before falling back to a real `lookup_method`
  walk — a genuine small polymorphic cache, not just a monomorphic one.
  Measured directly (`bench/dispatch_reassign_control.di` vs
  `bench/dispatch_polymorphic_no_index.di`, isolating dispatch cost from
  both the original `dispatch_polymorphic.di`'s `Array#[]` confound and
  the receiver-selection control flow itself): a 4-class call site lands
  1,999,996/2,000,000 real cache hits after a 4-call warmup and costs
  ~1ns/call more than a forced-monomorphic control with identical
  control flow — noise-level, despite never getting the `INVOKE_MONO`
  bytecode rewrite. The genuine "slow path every call" only appears past
  the cache's own width: `bench/dispatch_megamorphic.di` (6 classes)
  shows 0 hits, 2,000,000 misses. So there's no 2-4-shape gap to fix —
  the existing cache already handles exactly that case. What's actually
  unmeasured (and would be the real next question, lower priority, not
  started) is miss cost on a realistic class hierarchy with superclass
  chains once a site exceeds width 4 — this round's megamorphic
  benchmark uses flat classes, so `lookup_method`'s fallback walk was
  cheap in a way a deep hierarchy might not replicate.

### Fibers and concurrency

- Fiber lifecycle states, frame checkpoints, a FIFO scheduler with run-once
  and run-all draining, and GC roots for queued/suspended fiber frames.
- Rewrote fibers to be stackful, each on its own native OS stack via POSIX
  `ucontext`, fixing two correctness bugs in the earlier interpreter-level
  checkpoint approach (a nested `yield`'s progress being lost on resume, and
  an active `rescue` handler stack not surviving a suspend).
- Diamond-language fiber syntax: `Fiber.new(callable)`, `yield`/`yield(value)`
  as an ordinary expression, `.resume(value)`/`.status()`/`.alive?()`, and a
  rescuable `FiberError` for resuming a completed/failed fiber.
- `Thread`: real OS-level parallel execution via `pthread_create`, each
  thread running against a fully independent heap (its own `DiamondVm` and a
  byte-for-byte cloned `DiamondProgram`) rather than making the existing
  single-threaded GC/dispatch/inline-cache machinery thread-safe.
  `Thread.new(callable, *args)`/`.join()`/`.alive?()`, cross-heap argument/
  result copying via a generalized `copy_value_into_vm` (shared with
  `ProgramBuilder#run`'s own cross-heap `Instance` handling), a GC block-join
  guarantee so no OS thread outlives its handle, and a `make test-tsan`
  ThreadSanitizer build variant. See `docs/threads.md`.
- Added the documented `__sanitizer_start_switch_fiber`/
  `__sanitizer_finish_switch_fiber` annotation pairs around every
  `swapcontext` call (`diamond_fiber_run`/`diamond_fiber_trampoline`/
  `DIAMOND_OP_YIELD`), with real destination stack bounds (via
  `pthread_getattr_np` for the native/main thread, or a resuming ancestor
  fiber's own mmap region for nested resumes) rather than the docs'
  permissive NULL-bounds fallback.
- Fixed a real dangling-frame-chain bug this investigation turned up along
  the way: `mark_object`'s `DIAMOND_OBJECT_FIBER` case and the scheduler's
  `mark_fiber` both unconditionally marked `fiber->native_frames` as a GC
  root. That field is only refreshed when a fiber actually suspends or
  completes (in `diamond_fiber_run`, right after its `swapcontext` returns)
  -- while a fiber is `DIAMOND_FIBER_RUNNING`, it's a stale snapshot from
  the *previous* suspend, and once the fiber's execution progresses further
  than that snapshot (real recursive `run_chunk` calls genuinely returning
  and popping their C stack frames), it points at frames that no longer
  exist. Both call sites now skip a fiber's `native_frames` while it's the
  currently-running one; its true live frames are already covered by
  `diamond_vm_collect`'s own `mark_frame_chain(vm->frames)` (if it's the
  innermost running fiber) or by an ancestor's `resumer_frames` snapshot
  (if it's a fiber blocked resuming a nested child) -- both already walked
  there. Confirmed with a minimal repro (one fiber, recursed ~30 native
  frames deep, resumed a second time under `DIAMOND_STRESS_GC=1`, mid-unwind
  allocation triggers a collection) that reliably reproduced a
  `stack-use-after-return`/`heap-use-after-free` under ASan before the fix
  and is clean after it, on both GCC's libsanitizer and LLVM's compiler-rt.

### Strings, numbers, and structured data

- `Float`, a single IEEE-754 double type with mixed `Int`/`Float`
  arithmetic/comparison auto-promotion, `to_f`/`to_i` conversions, exponent-
  notation literals (`1e10`), and shortest-round-trip formatting.
- Arbitrary-precision integers: `Int` arithmetic overflow auto-promotes to a
  heap-allocated bignum (no separate `BigInteger` type), canonicalizing back
  to a plain `Int` whenever it fits.
- Symbols (`:name`), content-compared and content-hashed, with `to_sym`.
- A `Math` library (`sqrt`/`sin`/`cos`/`tan`/`pow`) and numeric helpers
  (`abs`/`min`/`max`/`mod`) covering `Int`/`Float`.
- `Time.monotonic()`: a duration-only clock (`CLOCK_MONOTONIC` seconds
  as a `Float`) for timing an elapsed interval (`elapsed =
  Time.monotonic() - start`). Added for `examples/library`'s rack
  timing middleware.
- A real calendar `Time` type (`Time.now`/`.utc_now`/`.at`, component
  accessors, `.strftime`, UTC/local conversion via `gmtime_r`/
  `localtime_r`) with genuine Ruby-style operators (`t + n`, `t1 - t2`,
  full comparisons) — the one native, non-`Instance` type with operator
  support, implemented directly in the VM's arithmetic/comparison
  opcodes rather than through the class-method operator-overload
  mechanism. See `docs/io.md`'s "Time" section.
- `Regexp.new(pattern, options)`/`.match`/`.match?`, backed by a separate
  regex-engine project (`reginold`).
- String primitives: `upcase`/`downcase`/`reverse`/`strip`/`split`/`slice`/
  `index_of`/`to_i`/`to_f`/`ord`/indexing (`"abc"[0]`)/`repeat(n)`/
  `start_with?`/`end_with?`/`include?`/`capitalize`/`chars`/`bytes`/`chomp`/
  `ljust`/`rjust`/`downcase`/`tr`, plus `chr(code)` and `Int#chr` for
  building strings from byte values.
- `String#gsub`/`#sub`/`#scan` using `Regexp`, including `\1`-style
  backreference substitution in the replacement string.
- A `JSON` module (`JSON.stringify`/`JSON.parse`), pure Diamond, with
  `\uXXXX` escape support.
- `String#format`, a `sprintf`-style formatter (`%d`/`%i`/`%f`/`%x`/`%X`/
  `%o`/`%b`/`%s`/`%%`, with `-`/`0`/width/precision) taking a single value
  or an `Array` of them rather than needing variadic/splat call support,
  which Diamond doesn't have. See `docs/syntax.md`. One of the release-
  readiness gaps a "what do most languages have that Diamond doesn't"
  pass turned up (no format-string method at all, previously).
- The `<<` operator: another release-readiness gap from that same pass —
  there was previously no lexer token for it at all. `Int << Int` is a
  bitwise left shift; `Array << value` pushes and returns the array
  (Ruby's chainable append idiom). Native-only on those two types, not
  user-overloadable, same as `Time`'s operators — see `docs/syntax.md`'s
  "Operator overloading" section. New precedence tier (`PREC_SHIFT`,
  between comparisons and `+`/`-`, matching Ruby) in both the native
  compiler and `selfhost/parser.di`'s mirror. Landed the self-hosted side
  first with the wrong numeric opcode value (hand-counted the gap between
  `PROGRAM_BUILDER_NEW` and the new opcode, missed `SQLITE3_OPEN` in the
  count) — `tests/parser_diff.sh` caught it immediately as a "constructed
  bytecode is invalid" failure once a `tests/parser_cases/*.di` fixture
  actually exercised the new operator; re-derived the correct value with
  a throwaway C probe instead of counting by hand a second time.

### Collections and Enumerable

- Replaced `Hash`'s O(n) linear-scan lookup with a real open-addressing hash
  table, keeping the existing insertion-ordered entry array.
- Core collection/numeric helpers (`lib/core.di`): Array `reverse`/`concat`/
  `compact`/`uniq`/`flatten`/`join`/`delete_at`/`array_sort` (Int-only), Hash
  `merge`.
- Real `Enumerable`: `select`/`count`/`any?`/`all?`/`reduce`/`map` derived
  from `each`, available on Array/Hash receivers and via `include Enumerable`
  for user classes.
- Enumerable completeness: `sum`/`sort`/`sort_by`/`reject`/`find`/
  `each_with_index`/`min`/`max`, with `sort`/`sort_by`/`min`/`max` working on
  any class with `<`/`>` overloaded.
- `Array#join` became a genuine native, `StringBuilder`-backed O(n) method
  (it was a Diamond-level `result = result + piece` loop in `lib/core.di`
  before -- exactly the O(n^2) concatenation pattern the pre-release audit
  flagged) and `StringBuilder` (`lib/core.di`) is now a named, discoverable
  escape hatch from that pattern for arbitrary incremental string building,
  not just pre-collected arrays.

### Self-hosting (Diamond-in-Diamond)

- Phase 0: raised core VM capacity limits (functions, classes, methods,
  fields, and more) to fit a compiler-sized Diamond program; fixed a hidden
  stack-overflow risk from the resulting program-struct size, and corrected
  two limits that had been raised past the one-byte bytecode-index ceiling
  they're bound by.
- Phase 1: `ProgramBuilder`, a native bridge letting Diamond code assemble
  and run a `DiamondProgram` at runtime — the foundation for a Diamond-
  language compiler to produce anything executable.
- Phase 2: `selfhost/lexer.di`, a full Diamond-language port of the lexer,
  verified byte-for-byte against the native lexer across the entire test
  corpus.
- Phase 3: `selfhost/parser.di`, a Diamond-language port of the compiler,
  built up incrementally to cover expressions and control flow; functions,
  closures, and calls; classes, inheritance, and `super`; the full gradual-
  typing surface (scalars, unions, collections, callables, interfaces,
  generics, and flow narrowing); exceptions/rescue/ensure; and modules,
  namespaces, and `require`, each verified against the native compiler via a
  growing differential test harness (hundreds of matching cases).
- Bootstrap milestone: the self-hosted parser compiles and correctly runs
  its own two source files, and a further check confirms the self-compiled
  parser can itself compile and run a third, independent program.
- Widened function indices from 8 to 16 bits (`DIAMOND_MAX_FUNCTIONS`
  256 → 512) to give the self-hosted bootstrap compile headroom.
- Widened register operands from 8 to 16 bits (the per-function register
  ceiling, 256 → 4096) for the same reason, hit repeatedly during
  self-hosting. `run_chunk`'s register array became a VLA sized to each
  function's own high-water mark instead of a fixed
  `DIAMOND_REGISTER_COUNT`-wide array, so ordinary small functions use
  *less* stack than before and only a function that actually needs
  hundreds/thousands of registers pays for the wider frame.
- Widened `ProgramBuilder#run` to return `String`/`Symbol`/`Bignum`/`Array`/
  `Hash` results (deep-copied across the isolated builder VM boundary), not
  just scalars.
- Running real, previously-unrunnable self-hosted-compiled programs for the
  first time surfaced and fixed a handful of genuine native-compiler bugs
  (a defaulted-parameter register-allocation bug and an over-permissive
  interface-conformance check, both in the self-hosted port; the closure-
  capture bug noted under "Modules, metaprogramming, and dispatch").
- Lifted the self-hosted parser's own one-level nested-closure-depth
  restriction; it was stale, not load-bearing.
- Fixed a real desync bug: `selfhost/parser.di`'s own `Opcode` module
  hand-mirrors `src/vm.h`'s opcode enum with literal numbers, and every
  entry from `REGEXP_NEW` onward had silently drifted 7 values stale after
  UDP/Signal/TLS opcodes were inserted earlier in the native enum without a
  matching update here — miscompiling, among other things, every
  self-hosted-compiled program's own `ProgramBuilder.new()` calls into the
  wrong opcode. Also removed a stale parser-error-case fixture
  (`rescue_generic_type`) left over from before generic type variables
  were allowed to filter `rescue`, which the differential test suite had
  never actually re-verified since (masked by the opcode bug above
  aborting the run before reaching it).
- `Instance` results can now cross a `ProgramBuilder#run` boundary. The
  real blocker turned out deeper than a missing `copy_value_into_vm` case:
  a `DiamondMethod.function_index`/superclass index is only meaningful
  against *its own* program's tables, and every dispatch site resolves
  those against whichever chunk is running the call, not anything
  per-instance — so a naive copy would call through the wrong function
  table entirely. Fixed by giving `DiamondInstance` an `owner` field
  (nullptr for an ordinary instance, meaning "resolve against whatever
  chunk is ambient," true everywhere already); `copy_value_into_vm`
  now handles `DIAMOND_OBJECT_INSTANCE` by adopting the *whole* source
  program into the receiving `DiamondVm` (`DiamondVm.adopted_programs`,
  kept alive for the rest of that vm's lifetime, lazily and only once per
  result that actually contains an instance) rather than trying to copy
  or relink the class and its methods' bytecode — and pointing the copied
  instance's `owner` at the adopted program's own stable chunk. Dispatch
  sites (`INVOKE`/operator overloads/`to_s`) read a receiver's `owner`
  instead of trusting the ambient chunk to make this work. Deliberately
  doesn't attempt `is_a?`/`case`/duck-typing checks against a *local*
  class for a crossed-over instance (memory-safe but not meaningful, since
  the foreign class is a genuinely distinct type from anything locally
  defined) — only calling the instance's own methods was in scope.
- Fixed a feature-parity gap `make test-lexer-diff` caught: `selfhost/
  lexer.di` never learned about `@@name` class variables, so it split
  `@@x` into an `error` token plus an `instance_variable` token instead
  of one `class_variable` token the way the native lexer already did.
  `tests/lexer_dump.c`'s own `kind_name()` table had the same gap
  (printed `<unknown>`). Also surfaced a second, unrelated self-hosted
  gap along the way: `selfhost/parser.di` doesn't support an expression
  split across a line right after `&&` (the native compiler's own fix
  for this, see "Control flow, exceptions, and the module loader" above,
  was apparently never ported) — the class-variable fix itself is
  written to avoid that shape rather than fixing the parser gap, which
  stays open.

### Language server (LSP)

- `lsp/`, a v1 diagnostics-only Language Server Protocol implementation
  (own binary, `Content-Length`-framed JSON-RPC over stdio), recompiling an
  open document through the compiler on every change; fixed to account for
  `require`, which is resolved by the loader rather than the parser.
- A syntax-highlighting-only VS Code extension (TextMate grammar), later
  given a real hand-rolled LSP client (`extension.js`) that spawns and talks
  to `diamond-lsp`.
- `textDocument/hover` and `textDocument/definition`/`documentSymbol` for
  top-level functions and classes (the two identifier kinds resolvable
  without a full symbol table).
- Diagnostics for a broken `require`d file now publish against that file's
  own document instead of nowhere.
- Live in-memory `require` resolution (an open, unsaved document's current
  buffer is used instead of stale on-disk content) and dependency-aware
  republishing when a required file's live content changes.
- `textDocument/completion` and `workspace/symbol`, backed by a new
  persistent per-function scope table (`DiamondFunction.scope_locals`)
  recorded as a side effect of compiling.

### Native I/O: files, sockets, signals, TLS

- `print`/`puts` and `gets` (stdin), sharing the same stringification path
  as string interpolation.
- `File.open(path, mode)` with `.read`/`.gets`/`.write`/`.close`, and a new
  `IOError` exception class for I/O failures.
- Blocking TCP sockets: `TCPSocket.connect`/`TCPServer.listen`/`.accept`,
  sharing `File`'s read/write surface via `fdopen`.
- A basic Rack-style HTTP server library (`http_serve`) and the String
  primitives it needed (`index_of`/`slice`/`to_i`/`downcase`), plus an HTTP
  client (`http_get`/`http_post`/`http_request`).
- Capped how much of a peer's claimed `Content-Length` `http.di` will
  actually read (`http_max_body_size`, 10 MiB) -- the pre-release audit
  flagged the previously-unbounded read as a memory-exhaustion DoS. The
  server drops an oversized request (same "connection closed early"
  path `http_serve`'s accept loop already handles, so one bad request
  can't crash the whole server); the client raises `IOError`, since a
  single outbound call has no next connection to move on to.
- Non-blocking sockets (`TCPServer.listen_nonblocking`, `Socket`, `IO.poll`)
  and a fiber-per-connection concurrent HTTP server (`packages/gremlin`)
  built on top, giving fibers real concurrent I/O.
- UDP sockets: `UDPSocket.bind`/`.open`/`.send`/`.receive`.
- `Signal.trap(name, handler)` for `INT`/`TERM`/`HUP`, including correct
  handling of a signal delivered while blocked in `accept`/`IO.poll`/
  `receive`.
- TLS: `TLSSocket.connect`/`TLSServer.listen`/`.accept`, built on OpenSSL,
  with mandatory certificate/hostname verification.

### REPL

- A REPL (`diamond` with no arguments on a terminal), replaying and diffing
  an accumulated session buffer on every evaluation since the VM has no
  incremental-compile mode.
- Interactive line editing: in-place cursor movement, Backspace/Delete,
  Up/Down history (persisted to `~/.diamond_history`), and Ctrl-C that
  aborts only the current input.
- A bare `exit`/`quit` typed at the prompt exits too, not just Ctrl-D —
  recognized only as the first line of a fresh statement, so it can't
  misfire mid-continuation.

### Testing library

- `lib/minitest.di`: `assert`/`assert_equal`/`refute`/`refute_equal`/
  `assert_nil`/`assert_raises`, test registration, and a pass/fail/error
  runner with a summary line and non-zero exit on failure.
- Rounded out with `refute_nil`, `assert_includes`/`refute_includes`,
  `assert_empty`/`refute_empty`, `assert_in_delta`, `Minitest.skip`, and
  per-suite `setup`/`teardown` hooks.
- `assert_raises[E]`, checking a specific exception type via an explicit
  generic argument.

### Tooling and performance

- `facet`, a standalone package-manager CLI (git-based fetch/install/
  lockfile), plus built-in `require` fallback resolution for installed
  packages (`diamond_packages/name`) with optional manifests.
- A libFuzzer harness for the compiler front end (`fuzz/compile_fuzzer.c`).
- GitLab CI, which found and fixed two real cross-filesystem portability
  bugs (a directory-as-file assumption that didn't hold on the CI runner's
  filesystem).
- Test-suite speed: a batch runner that reuses one process and one compiled
  prelude across the whole `tests/cases/*.di` corpus instead of spawning a
  fresh process per case, roughly halving full test-suite time.
- A per-input execution watchdog for `fuzz/execute_fuzzer.c`: it feeds
  raw bytecode straight into `run_chunk`, which enforces no execution-
  step budget by design (real Diamond programs legitimately run
  unbounded loops), so a trivial self-jump (`DIAMOND_OP_JUMP` to its own
  offset) hung for libFuzzer's full default 1200s timeout and was
  reported as a "crash." Fixed with a `sigsetjmp`/`siglongjmp` cutoff
  around each `diamond_vm_run` call, timed by a dedicated POSIX timer on
  `SIGUSR1` rather than `alarm()`/`SIGALRM` — sharing that signal with
  libFuzzer's own internal `-timeout` watchdog risked a stray signal
  firing outside this harness's protected window and jumping into a
  dead `jmp_buf`. Verified stable across a real, sustained fuzzing run
  (8500+ executions) with the self-jump case seeded directly into the
  corpus.

## Later experiments

- Self-hosting the compiler and core libraries in Diamond: the core
  bootstrap (lexer, parser, and a self-compiling, self-running bootstrap
  check) has landed — see "Self-hosting (Diamond-in-Diamond)" above. What
  remains is open-ended: any narrower native/self-hosted compiler feature-
  parity gaps a future differential sweep might still turn up.
- Native-code generation or a tracing/method JIT — nothing shipped so far
  generates native code; it's all interpreter-loop tuning (register
  zero-init, opcode dispatch, struct-copy elimination). Actual JIT
  compilation remains a distinct, larger, not-yet-attempted piece of work.

## Explicitly deferred

- Ruby compatibility (not a goal; only familiar syntax and object conventions).
- Stable bytecode and embedding APIs -- `ProgramBuilder` (the concrete
  instance of this the pre-release audit flagged as undecided) stays an
  internal mechanism the self-hosted compiler bootstrap needs, not a
  supported embedding surface; see `src/object.h`'s `DiamondProgramBuilder`
  comment for the full reasoning.
- Multi-platform support.

## Inconclusive

Nothing currently open — the last entry here (an ASan `stack-use-after-
return` inside `mark_frame_chain`, seen once during manual fiber+ASan
testing) turned out to be a real, reproducible dangling-frame-chain bug
rather than a sanitizer artifact, and was fixed; see "Fibers and
concurrency" above.

## Judgement calls

- **Bare-name forward/mutual recursion between top-level functions (or
  sibling methods calling each other by bare name) fails to compile.**
  Two functions or methods that call each other by bare name, where one is
  declared before the other, fail with `undefined function`/
  `undefined local variable`, because a bare call resolves only against
  locals and already-declared functions at compile time, in file order.
  The workaround (`self.method(...)`, which resolves at call time against
  the runtime class) is used throughout the codebase's own Diamond code.
  This is a debatable one to list: the project's own documentation
  explicitly declined to treat it as a bug ("a real, reproducible compiler
  behavior, not a bug filed here"), so it may be an accepted consequence of
  single-pass compilation rather than something intended to be fixed.

- **Generational or incremental GC.** `diamond_vm_collect` (`src/vm.c:299`)
  is a plain stop-the-world mark-and-sweep: every object (String, Array,
  Hash, Instance, Closure, Cell) is individually `malloc`'d onto one
  intrusive linked list, collected with a doubling threshold, no nursery
  for short-lived allocations, and every collection re-walks the entire
  live heap regardless of how much of it is actually garbage. Flagged by
  the pre-release audit as fine for scripts/short-lived processes but a
  real ceiling for anything long-running with a large live set --
  `packages/gremlin`'s fiber-per-connection HTTP server is exactly that
  shape. `docs/gc-generational-design.md` maps out a design (non-moving,
  list-splice promotion, old→young write barrier) so that work doesn't
  have to be re-derived once it's picked up -- nothing in it is
  implemented yet.

  The long-running benchmark this entry used to call for as a
  prerequisite now exists: `bench/burn_in` runs a real
  `gremlin_serve(..., threads: 4)` server under sustained `ab` load, with
  a per-worker session cache (`bench/burn_in/server.di`) sized to hold
  20000 live, continuously-mutated records per worker -- a genuinely
  large live set, not a hypothetical one. A 600s run against it (see
  that directory's README for the full numbers) showed RSS holding
  steady in a 4.2-5.1GB band with no growth trend, and flat ~6500-7000
  req/s throughput with 2-3ms p99 latency, for the full 10+ minutes.
  Building `bench/burn_in` surfaced two unrelated real bugs along the
  way (a `gremlin_serve(threads: N)` hang from a GC/Thread interaction,
  and an `ab` flag gotcha in the harness itself, both fixed -- see that
  directory's README), which was worth it independent of what it did or
  didn't show about GC.

  A follow-up push at this same 20000/worker config -- re-run on both
  current `main` and, via a `git worktree`, the exact commit the 600s
  numbers above were recorded at -- found that single "representative"
  run was not, in fact, representative: RSS on *both* commits swings
  more than that 4.2-5.1GB band suggests and crossed 5.5GB within the
  first minute on more than one short re-run, ruling out a regression
  in the commits between (the behavior reproduces identically on old
  code) but also ruling out "flat and stable" as an accurate one-line
  summary of this workload's real memory behavior. What's still
  genuinely unknown: whether that swing is bounded noise around a
  higher-than-documented steady state, or a slow climb the original
  600s run's own lucky trajectory happened to mask -- distinguishing
  those needs a longer run than got attempted, which didn't happen this
  round for a specific reason: an earlier, unsupervised attempt at
  literally the same question, run at 3x the live-set size with no hard
  memory ceiling, consumed all RAM and swap on the single machine this
  project runs on and crashed it. `bench/burn_in/run_hard.sh` now
  exists specifically to make a repeat of that impossible -- an active
  watchdog polls the server's RSS every second (independent of the load
  generator's own batch boundaries) and `kill -9`s it the instant a
  hard, pre-computed-from-`free`-headroom cap is crossed, aborting with
  a clear message rather than continuing past the cap. Both the
  confirmation runs above ended via that watchdog firing as designed,
  not via approaching any real danger.

  Three consecutive watchdog-protected runs at this same config each
  crossed a progressively higher cap progressively faster (5.5GB in
  under a minute, twice, then 6GB in under 20 seconds), which is a
  strange enough pattern to distrust rather than chase with a fourth,
  still-higher-cap attempt -- checked for the obvious mundane
  explanation (CPU thermal throttling skewing successive runs) via
  `/sys/class/thermal` and `/proc/loadavg` and ruled it out (temps
  under 45C, load average 1.4, nothing throttled). Left unresolved
  between "genuinely high run-to-run variance from GC/thread-scheduling
  nondeterminism under concurrent load" and "something about this
  session's cumulative machine state" -- distinguishing needs cleaner
  evidence than more live runs can give.

  `vm->gc_collection_count`/`gc_total_seconds` (`src/vm.h`, timed in
  `diamond_vm_collect`, `src/vm.c`) now exist for exactly that: direct
  GC-cost evidence (`DIAMOND_TRACE_GC=1`, printed the same way every
  other `DIAMOND_TRACE_*` counter is, `src/run_source.c`) instead of
  inferring collector behavior from external RSS sampling under live
  `ab` load, which conflates request-handling timing, OS scheduling,
  and page-cache behavior with the collector's own cost. Not yet wired
  up to anything a long-running, signal-killed `gremlin_serve` process
  can surface, though -- an ordinary Diamond program reaches the normal
  print-at-exit path these counters use, but `bench/burn_in`'s server
  never exits cleanly (it's always `kill`ed). Giving the live burn-in
  case a way to read these mid-run (a periodic print, a signal handler,
  or exposing them to Diamond code some other way) is a separate,
  not-yet-designed follow-up. Whoever picks this up next should use
  `run_hard.sh` (not the original `run.sh`, which has no watchdog) for
  any further live-server push, choose its cap from actual `free -h`
  headroom at launch time -- not a guess -- but consider reaching for
  `DIAMOND_TRACE_GC` on a shorter, non-networked, non-daemon reproducer
  first, since it sidesteps this whole class of confound.
