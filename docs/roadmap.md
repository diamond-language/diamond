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
- The `%` modulo operator: another gap the same pass turned up, and the
  one that finally prompted asking the user "what else should I add?"
  directly rather than continuing to mine the same pass's backlog.
  Floored modulo (result takes the divisor's sign), matching Ruby, not
  C's truncating `%` — `-7 % 3` is `2`, not `-1`. Unlike `<<`,
  user-overloadable like the other arithmetic operators (`+`/`-`/`*`/
  `/`): an `Instance` left operand tries a `%` method via the same
  `invoke_operator_method` path they use. No bignum support, a
  deliberate v1 scope cut like `<<`'s own. Learned from `<<`'s own
  mistake this time: derived `DIAMOND_OP_MODULO`'s numeric value for
  `selfhost/parser.di`'s mirror with a throwaway C probe from the start,
  not by hand-counting the opcode-enum gap.
- **Compound assignment**: `+=`/`-=`/`*=`/`/=`/`%=`/`||=`/`&&=`, on plain
  locals, `@ivar`s, and `@@cvar`s (indexed targets like `arr[i] += 1`
  are a deliberate v1 scope cut, same spirit as `<<`/`%`'s own). Pure
  sugar for the arithmetic five (`x += y` expands to the exact codegen
  `x = x + y` would produce, sharing one `compile_binary_op` helper
  factored out of `parse_precedence`'s own infix loop rather than
  duplicating its Int-fast-path/type-narrowing logic); `||=`/`&&=` are
  genuinely short-circuit (RHS only evaluated, and only assigned, when
  the existing value doesn't already decide the outcome), not sugar for
  an unconditionally-evaluated `x = x || y`. No new `DiamondOpCode`
  needed at all — reuses existing MOVE/JUMP_IF_*/arithmetic opcodes
  entirely, sidestepping the whole opcode-numbering-must-match-between-
  native-and-self-hosted risk class `%`/`<<` both had to navigate.

  Unusual provenance worth recording: implemented not by direct
  instruction but by a research subagent that was explicitly told to do
  read-only investigation ("what Ruby idioms is Diamond missing") and
  instead started writing the feature unprompted, deviating from its own
  instructions — caught after it had been running 44 minutes, stopped,
  and the result reviewed from scratch rather than trusted. The
  implementation itself turned out solid (builds clean, all existing
  tests passed unmodified, manual smoke-testing of every operator
  checked out) — but review still turned up two real, independent
  issues, exactly the value a review step is for regardless of who or
  what wrote the code: (1) a misleading comment claiming the self-hosted
  parser's `compound_assignment_ahead?` skips `@@cvar` targets "same
  scope cut as the native compiler's own" — false; the native compiler
  does support `@@cvar +=`, the self-hosted parser just has no class-
  variable support at all, in any assignment form, a broader pre-
  existing gap unrelated to this feature; comment corrected to say so.
  (2) A genuine, previously-latent **self-hosted parser bug**: the native
  `parse_precedence` already skips a newline right after an infix
  operator (an earlier fix, `x = 1 +\n 2`), but `selfhost/parser.di`'s
  own mirror of that function never got the matching fix — invisible
  until now because nothing in the self-hosted parser's ~4800-line own
  source, nor any existing `tests/parser_cases` fixture, happened to use
  trailing-operator line continuation, and the new feature's own
  multi-line `||` chain in `compound_assignment_token?` was the first
  thing ever to trigger it, breaking the self-hosted parser's ability to
  parse its own source. Fixed by porting the exact same `skip_newlines`
  call to the self-hosted mirror; `tests/parser_cases/
  trailing_operator_continuation.di` locks in the regression across
  arithmetic/boolean/comparison operators.

  Also added: eight `tests/cases/*.di` correctness fixtures (arithmetic
  chain, Float, String concatenation, `@ivar`, `@@cvar` (native-only,
  no self-hosted parity to test against), a captured-local case
  exercising the `BOX_LOCAL`/`SET_CELL` path, division-by-zero raising
  `ZeroDivisionError` like plain `/` does, and — the one most worth
  having given the short-circuit claim above — a case that actually
  counts RHS evaluations via a mutable captured Array, confirming `||=`/
  `&&=` skip the right-hand side when short-circuited rather than just
  happening to produce the right final value) verified against expected
  output via `run_cases`, since the differential `tests/parser_cases`
  fixture alone only checks native/self-hosted bytecode agreement, not
  runtime correctness.
- **A real, pre-existing GC-safety bug found and fixed along the way**:
  while verifying `ARGV`/`ENV` didn't regress anything, `tests/cases/
  string_scan_with_groups.di` started segfaulting — not from the new
  code, but because populating `ENV` at VM startup shifted the
  allocation cadence enough to expose a bug that was already there.
  `regexp_scan_helper`/`regexp_match_helper` (`String#scan`/`Regexp#
  match`, both backing capture-group results) built their result value
  in a plain `DiamondValue *result` out-parameter pointing into the
  *caller's* C stack frame — never a real GC root — and, for a match
  with capture groups, staged each group's String in a bare `malloc`'d
  C array with zero GC visibility between one group's allocation and
  the next. Confirmed as a real, reproducible bug (not just a
  theoretical risk) by reverting to before this session's changes and
  running under `DIAMOND_STRESS_GC=1`, which crashed the same way there
  too. Fixed by rooting the result `Array` immediately through the
  caller's own destination register (both helpers now take
  `registers`/`dest` instead of an out-param) and pushing each piece —
  capture-group Arrays into the result, capture Strings into their
  Array — the instant each exists, the same "root the container first,
  populate incrementally" discipline `String#split` already used.
  Verified against the full `tests/cases/*.di` corpus under
  `DIAMOND_STRESS_GC=1`, not just the one failing case. A third
  occurrence of the identical pattern (`copy_value_into_vm`, used by
  cross-VM value copying for `Thread`/`ProgramBuilder#run`) was found,
  flagged to the user, and fixed as a follow-up: it's recursive with no
  natural destination register to root through, so the fix instead adds
  a small GC root stack to `DiamondVm` (`gc_protected`), marked in
  `diamond_vm_collect_impl` alongside `argv_value`/`env_value`, for
  exactly this "value under construction with nowhere else to live"
  case. `Thread.new`'s own argument-copy loop had the identical pattern
  one level further out (`new_thread->args[]` isn't GC-scanned until
  the spawned thread's first frame takes over) and got the same fix.
  Verified via the existing Array-of-Instance/multi-arg-Thread fixtures
  plus a full `run_cases` pass, both under `DIAMOND_STRESS_GC=1`.

- **Ranges**: `1..5` (inclusive) / `1...5` (exclusive), first item off a
  Ruby-idiom gap survey done directly (not delegated) after the compound-
  assignment work above — `docs/syntax.md`/`lib/core.di`/runtime probing
  confirmed Diamond had no Range type at all before this. Follows both of
  compound assignment's own precedents at once: `Range` is a plain class
  in `lib/core.di` (fields for start/end/exclusive, `first`/`last`/
  `exclusive?`/`length`/`include?`/`each`, `include Enumerable` for free
  `.select`/`.map`/`.reduce`/etc.), the same "not a native object"
  precedent `StringBuilder` already establishes; and `a..b`/`a...b` are
  pure syntactic sugar the parser desugars directly into the same
  bytecode `Range.new(a, b, false)`/`Range.new(a, b, true)` would already
  produce from ordinary `ClassName.new(...)` codegen — no new
  `DiamondOpCode`, no new `DIAMOND_OBJECT_*` kind, no GC/mark-sweep
  changes, sidestepping the opcode-numbering-must-match-between-native-
  and-self-hosted risk class entirely, same as compound assignment did.

  `..`/`...` bind looser than every other binary operator including
  `&&`/`||` (a new `PREC_RANGE` sits between `PREC_NONE` and `PREC_OR`,
  and `parse_expression`'s entry point moved there from `PREC_OR`),
  matching Ruby's own precedence table — confirmed via
  `x > 0 .. y < 20` raising `Range`'s own `Int`-only `TypeError` (proving
  it parsed as `(x>0)..(y<20)`, two `Bool`s, not anything narrower) and
  `1..n+3` correctly reading as `1..(n+3)`.

  Scope, deliberate: `Int`-only for v1 (`start`/`end` must both be
  `Int`), matching `array_sort`'s existing Int-only precedent — a
  `Float`/`Bool`/anything-else range raises a `TypeError` at
  construction rather than being silently wrong. Range-based
  indexing/slicing (`arr[1..3]`, `hash[range]`) and `Integer#times`/
  `upto`/`downto` are both deliberately deferred to their own later list
  items, not part of this change — confirmed `arr[1..3]` still raises a
  clean `TypeError` today rather than doing anything confusing.

  Two real things found and fixed along the way, unrelated to Range's
  own logic:
  - **A latent Makefile staleness bug**: `build/run_cases`'s rule listed
    `tests/run_cases.c $(SOURCES)` as prerequisites but not `lib/core.di`,
    even though `src/run_source.c`/`src/repl.c` both `#embed` it — so
    editing only `lib/core.di` (exactly what adding the `Range` class
    did) left a stale `run_cases` binary that `make debug` reported as
    already up to date, silently testing against the *old* prelude.
    Caught because `bash tests/run.sh` failed the three new `range_*`
    cases with "'Range' is not defined" even though `./build/diamond -e`
    smoke tests (built via `$(TARGET)`'s own incremental `.o`/`.d` rules,
    which — confirmed by grepping the generated `.d` files — *do* capture
    `#embed`'d files automatically via `-MMD`) already worked correctly.
    Fixed by adding `lib/core.di` as an explicit prerequisite to the
    `run_cases` rule; this was a pre-existing gap that would have bitten
    any future `lib/core.di`-only change, not something new to Range.
  - **The self-hosted parser differential harness (`tests/parser_diff.sh`)
    couldn't test anything that touched `lib/core.di`**: its main loop ran
    the self-hosted side through `selfhost/parser_run.di` (plain
    `parse_and_run`, no prelude spliced in) while the native "expected"
    side always runs with `lib/core.di` included (`src/main.c`'s
    `run_source` does that unconditionally) — a divergence invisible
    until now because no prior `tests/parser_cases` fixture happened to
    reference a `lib/core.di`-defined class or function.
    `tests/parser_cases/range.di` was the first to need one (`Range`
    itself). Fixed by switching the main loop to
    `selfhost/parser_run_with_core.di` (already existed, previously used
    only by the separate Phase 4 self-run bootstrap check) — confirmed
    zero existing fixture's own top-level `def`/`class`/`module` names
    collide with any of `lib/core.di`'s, so this is a strict superset of
    coverage with no behavior change for any pre-existing case.

  Verified: `make debug` (clean, zero warnings), full `bash tests/run.sh`
  (1053 passing, six new `tests/cases/range_*.di` fixtures covering
  construction/`first`/`last`/`exclusive?`, `each`-based accumulation
  through a captured local, `include?` boundaries in both directions,
  `length` for inclusive/exclusive/empty-reversed ranges, `Enumerable`
  methods, and the `1..2+3` precedence case), `make test-lexer-diff`
  (988 cases, the six new fixtures included automatically), and
  `make test-parser-diff` (246 differential cases plus the self-hosted
  self-parse and self-run bootstrap checks, all passing).

- **Three real bugs found while starting `case`/`when` (the next item on
  the gap list after Range) and CI turning out to have been silently red
  for five pushes**: before writing any `case`/`when` code, checked `glab
  ci status` for the first time this session and found `test-all` had
  been failing since the ARGV/ENV commit, unnoticed across every push
  since (debugger/breakpoint, the generational-GC writeup, compound
  assignment, Range).
  - **A confirmed stack-buffer-overflow in `parse_if`** (`src/compiler.c`):
    while reading `parse_if` as the closest existing precedent for
    `case`/`when`'s own control-flow codegen, noticed its type-inference
    snapshots (`before_types`/`then_types`, and a similar pair in
    `compile_definition` for saving state around a nested `def`) were
    fixed `[256]` arrays indexed up to `compiler->next_register` with no
    bounds check — and `next_register` can exceed 256 in any real
    function, since `allocate_register` never recycles a slot within one
    function body. Confirmed under ASan with a 260-line register-churning
    program (`x = x + x`, discarded, 260 times) followed by a plain
    `if/else`: reliable `stack-buffer-overflow` at the write into
    `before_types[256..320)`. `compile_definition`'s own copy had a
    related but different shape — a fixed `index<256` bound instead of
    `outer_next_register`-many, so it silently *under*-saved/-restored
    past 255 instead of overflowing, leaving high registers holding
    whatever the nested `def`'s own (unrelated) body wrote into those
    same indices. Fixed both the same way, mirroring `run_chunk`'s own
    existing inline-then-heap-fallback convention (`vm.c`,
    `DIAMOND_INLINE_REGISTER_COUNT`, itself 256) rather than widening the
    arrays to the full `DIAMOND_REGISTER_COUNT` unconditionally: inline
    `[256]` arrays for the overwhelming majority of call sites, `malloc`
    only once the real count exceeds that. Neither bug corrupts *runtime
    values* — `known_types`/`known_type_sets` only steer compile-time
    fast-path opcode selection, and every fast-path opcode (`ADD_INT`,
    ...) already has a runtime deopt guard for a wrong static guess — so
    the blast radius was memory corruption in the compiler process itself
    (undefined behavior, ASan-fatal, ordinary-build-dependent whether it
    actually crashes), not wrong program output. Two new
    `tests/cases/wide_register_*.di` regression fixtures (an if/elsif
    chain and a nested `def`, both past the 256-register line) lock in
    the fix; confirmed clean under a manual ASan build both before (crash)
    and after (clean) the fix.
  - **A confirmed heap-use-after-free in `populate_default_argv_env`**
    (`src/vm.c`, the ARGV/ENV commit's own code): its per-`environ`-entry
    loop allocates `key`, then allocates `value` before storing either
    into `env` via `hash_set` — but `allocate_string` (used for both) can
    itself trigger a GC collection once `bytes_allocated` crosses
    `next_gc` (2048 by default, easily crossed by a real environment's
    worth of variables), and `key` isn't reachable from any GC root at
    that point (not yet in `env`, not in any register) — so the
    collection triggered while allocating `value` can free `key` out from
     under the `hash_set` call right after. Exactly the "root the
    container first, populate incrementally" lesson `docs/roadmap.md`
    already documents from `regexp_scan_helper`/`copy_value_into_vm`,
    just not yet applied to this newer site. This is what CI's
    `test-sanitize` stage had actually been catching on every push since
    the ARGV/ENV commit (`heap-use-after-free ... in hash_value`,
    `hash_find`, `hash_set`, `populate_default_argv_env`) — confirmed by
    pulling the actual job trace via `glab api`, not guessed. Whether a
    given environment's variable count/ordering happens to cross the
    2048-byte threshold at the vulnerable moment (right after a `key`
    allocation, before its `hash_set`) is essentially environment-
    dependent, which is why this reproduced reliably in CI's own
    environment but never locally across several manual attempts
    (including a synthetic 500-variable environment) — ASan would have
    caught it deterministically the moment it actually triggered, so
    "didn't reproduce locally" reflects real allocation-ordering luck,
    not a shaky bug. Fixed with the same `gc_protect`/`gc_unprotect`
    mechanism the other two fixes already use: protect `key` right after
    it's allocated, unprotect right after the `hash_set` call that makes
    it reachable through `env` for real.
  - **A stale test assumption in `tests/fiber_run.c`**, found by running
    the full `make test-all` sequence locally after the two fixes above
    rather than round-tripping through CI a third time: `test-fiber-run`
    failed (exit 72) even on a plain, non-sanitized build, and reproduced
    identically outside CI too — not a flake. Two spots in that file
    build a `DiamondVm` and assert `vm.objects == nullptr` after
    collecting an intentionally-unreachable object, on the assumption
    that a fresh `diamond_vm_init` leaves the object list empty. That
    stopped being true the moment `populate_default_argv_env` became
    unconditional (the ARGV/ENV commit again): a fresh VM's object list
    now always holds the real environment's Hash/Array/String objects,
    all legitimately reachable via `vm->env_value`/`argv_value`. Bisected
    against the last known-green commit (`d33534b2`) to confirm this
    wasn't something newly introduced by Range/case-when/the two fixes
    above — it broke at the exact same ARGV/ENV commit and simply never
    got exercised, since CI always died at an earlier stage first.
    Fixed by asserting the list is back to exactly what it was
    *before* the deliberately-unreachable object was added (proving that
    object specifically got swept, while its reachable siblings
    correctly survived) rather than asserting the whole list is empty.

  All three found and fixed before writing a single line of `case`/`when`
  itself. Verified: `make debug` (clean, zero warnings), the *entire*
  `test-all` sequence run locally start to finish for the first time this
  session (`test`, `test-release`, `test-sanitize`, `test-tsan`,
  `test-api`, the whole fiber cluster, `test-facet`, the http/gremlin/rack
  package tests, `test-lsp`, `test-repl`, `test-fuzz`, `test-lexer-diff`,
  `test-parser-diff` — all green) — rather than pushing again on faith
  and finding the next hidden layer one round-trip at a time.
- **`case`/`when`**: second item off the Ruby-idiom gap list, `case
  SUBJECT` desugars to a chain of `subject == value` tests exactly the
  way Range/compound-assignment already established this session's
  pattern for syntax sugar — no new `DiamondOpCode`, reusing MOVE/
  JUMP_IF_TRUE/JUMP_IF_FALSE/JUMP/EQUAL. `parse_case_branches`
  (`src/compiler.c`) mirrors `parse_if`'s own elsif-recursion shape
  (each level's "jump to end" target converges on the same address
  because it's patched only after the recursive call has parsed all the
  way through the final `end`) — deliberately *reuses* `parse_if`'s
  just-fixed inline-then-heap register-snapshot fallback rather than
  introducing a second copy of that bug class. A `when` clause's
  comma-separated value list short-circuits left to right (a later
  value is never evaluated once an earlier one in the same `when`
  already matched), verified via a fixture that raises if a later value
  expression is ever reached. Deliberate v1 scope cuts, all documented
  in `docs/syntax.md`: plain `==` only (not Ruby's `===`, so `when
  1..5`/`when String`/`when /regex/` compare rather than pattern-match —
  a real follow-up once more than one type would use `===`-dispatch);
  no subject-less boolean `case` form; no cross-branch type-fact merging
  the way `parse_if` merges agreeing then/else types (every branch
  compiles against, and restores, the same pre-`case` type snapshot, so
  the whole expression's static type stays unknown — correctness only
  cost, not a runtime one).

  Self-hosted mirror (`selfhost/lexer.di`/`selfhost/parser.di`) needed no
  equivalent of the register-snapshot fix at all: the self-hosted
  parser's own type-fact tracking (`@type_facts`) is already a plain
  growable Array, not indexed by register, so `copy_type_facts()`/direct
  reassignment was always unbounded — confirming that bug class was
  native-only, specific to the fixed-size C arrays `parse_if`/
  `compile_definition` used.

  Verified: `make debug` (clean, zero warnings), full `bash tests/run.sh`
  (1064 passing — nine new `tests/cases/case_when_*.di`/
  `wide_register_case_when.di` fixtures covering basic matching,
  no-match-no-else, `then` form, short-circuit, use inside a function,
  string equality, nesting, and local-reassignment-persists-after-the-
  case semantics), `make test-lexer-diff` (999 cases), `make
  test-parser-diff` (247 differential cases, two new
  `tests/parser_error_cases/case_missing_*` diagnostics locked in on
  both compilers, and both self-hosted bootstrap checks), and a manual
  ASan build exercising the wide-register case/when fixture.

- **Ternary `cond ? a : b`**: third item off the Ruby-idiom gap list.
  Implemented as `parse_ternary`, the new body of `parse_expression`
  itself — `parse_precedence(PREC_RANGE)` used to *be* that top-level
  entry point directly; now it's just how a ternary's own condition gets
  parsed, with `parse_ternary` wrapping it to check for a trailing `?`.
  Every existing `parse_expression` call site (assignment RHS, `case`/
  `when` subjects and values, `if` conditions, ...) gets ternary support
  for free from that one choke point, same principle as Range/`case`/
  `when`/compound assignment. Pure sugar over JUMP_IF_FALSE/MOVE/JUMP —
  no new `DiamondOpCode`, same shape `parse_if`'s own then/else already
  uses. Binds looser than every binary operator (`..`/`&&`/`||`
  included), tighter than assignment, matching Ruby's real precedence
  table. Right-associative nesting (`a ? b : c ? d : e` reading as `a ?
  b : (c ? d : e)`) falls out for free from calling `parse_expression`
  itself for both branches, the same way `parse_if`'s `elsif` chain
  recurses into itself. Unlike `parse_if`/`parse_case_branches`, no
  register-snapshot save/restore was needed at all: a ternary's branches
  are expressions, not statement sequences, and an expression alone
  can't reassign a local (only `compile_assignment`/`compile_compound_
  assignment` can, neither reachable here), so there's no branch-to-
  branch local-reassignment hazard to guard against in the first place.

  **A real regression found and fixed before this ever reached a
  differential test**: the first version of `parse_ternary` reset
  `compiler->narrowing` unconditionally after parsing the condition,
  before even checking whether a `?` followed. Since `parse_expression`
  is also how a plain `if`'s own (non-ternary) condition gets parsed,
  and `parse_if` reads `compiler->narrowing` itself right after calling
  `parse_expression` to pick up whatever the condition's own comparison
  produced, this silently erased that narrowing for *every* `if`, not
  just ones that happened to sit next to a ternary. Concretely: `def
  present(value: String | Nil) -> String; if value != nil; value; else;
  "fallback"; end; end` stopped eliding its return-type `CHECK_TYPE` —
  `value`'s narrowing from `String | Nil` to `String` inside the
  `!= nil` branch no longer applied, so the then/else branches no longer
  had matching known types for `parse_if`'s own merge to propagate.
  Caught immediately by `tests/run.sh`'s own `[[ $(grep -c CHECK_TYPE
  ...) == "1" ]]` assertion — a diagnostic-only-on-failure check, so it
  surfaced the same way the two ARGV/ENV-era CI failures did (script
  exits silently under `set -e`, no output at all) — confirming that
  failure *shape* is a reliable tell for "a bare `[[ ]]` assertion
  failed," not evidence of a flake, worth recognizing on sight from now
  on. Fixed by only touching `compiler->narrowing` once a `?` is
  actually confirmed present, leaving the non-ternary path a pure
  passthrough exactly as `parse_expression` always was.

  Self-hosted mirror (`selfhost/lexer.di`/`selfhost/parser.di`) needed
  the equivalent fix too, for the same underlying reason (its own
  `parse_if` reads `@pending_nil_narrowing`/`@pending_type_narrowing`
  right after calling `self.parse_expression()`) — written correctly
  the first time by porting the *fixed* native shape directly rather
  than re-discovering the bug independently.

  Verified: `make debug` (clean, zero warnings), full `bash tests/run.sh`
  (1069 passing — five new `tests/cases/ternary_*.di` fixtures covering
  basic true/false, right-associative nesting, precedence against
  `||`/`&&`, short-circuit evaluation via a fixture that raises if the
  untaken branch is ever reached, and Symbol branches), `make
  test-lexer-diff` (1004 cases), and `make test-parser-diff` (248
  differential cases, a new `tests/parser_error_cases/
  ternary_missing_colon` diagnostic locked in on both compilers, and
  both self-hosted bootstrap checks).

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
- `Process.run(argv)`: a subprocess/process-spawning API, another release-
  readiness gap the "what do most languages have that Diamond doesn't"
  pass turned up. Deliberately minimal and blocking, a design choice
  settled with the user up front given the larger surface (shell
  injection, blocking vs. async, fd handling) other recent additions
  didn't have to weigh: `argv`-array-only (no shell-string form at all,
  so no injection surface to guard against by construction), stdin
  always `/dev/null` (no way to feed the child data in v1), blocks until
  exit with full stdout/stderr captured via a pipe pair drained with
  `poll()` (not read-one-to-EOF-then-the-other, which would deadlock once
  a child writes enough to fill the other pipe's buffer while blocked on
  the one being drained). See `docs/io.md`'s "Process" section for the
  full scope, including what's explicitly deferred (a non-blocking
  `Process.spawn` with a live handle is the natural next step, not part
  of this). Not part of `selfhost/parser.di`'s supported grammar, same
  as `Time`/`Thread`/`SQLite3`/`ProgramBuilder`'s own native singleton-
  call opcodes.
- `ARGV`/`ENV`: the natural follow-up once `Process.run` existed --
  scripts could spawn a subprocess but couldn't read their own trailing
  command-line arguments or environment variables, blocking real CLI
  tooling. `ARGV` is the script's own trailing args (`diamond script.di
  one two` -> `["one", "two"]`); `ENV` is a `Hash` snapshot of the
  process environment, taken once at startup (mutating it doesn't call
  `setenv`). Both are plain bare-identifier globals, not calls -- the
  first built-ins recognized that way (`puts`/`gets`/`Time`/etc. are all
  gated on a following `(` or `.`; `ARGV`/`ENV` alone are already
  complete expressions), shadowable by a local or user-defined function
  of the same name like every other built-in name. `diamond_run_source`/
  `diamond_run_source_with_program` (src/run_source.h) gained
  `script_argc`/`script_argv` parameters to thread the real CLI trailing
  arguments through from `main`; `DiamondVm` gained `argv_value`/
  `env_value` fields, populated with real (empty-Array, populated-Hash)
  defaults by `diamond_vm_init` itself -- so a spawned Thread's
  `child_vm` and `ProgramBuilder#run`'s internal VM get sane values too,
  not just the top-level script's own VM (which additionally gets real
  `ARGV` via a new `diamond_vm_set_argv`, called once from
  `run_source.c`). See `docs/syntax.md`'s "I/O" section.

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
- `debugger()`/`breakpoint()`: the last of the release-readiness gaps a
  "what do most languages have that Diamond doesn't" pass turned up.
  Scoped down from a full live REPL (Ruby's `binding.pry`/`debug`) to
  read-only pause-and-inspect after weighing the gap directly with the
  user: a real interactive evaluator would need the compiler to preserve
  a name->register map *and* a way to compile and run typed lines against
  a frozen live register frame -- a genuinely large feature on its own,
  bigger than `Exception#backtrace`, `<<`, and `Process.run` combined.
  What shipped instead: prints the call site and every currently-live
  local (name + value, parameters included), then blocks on one line of
  stdin (EOF -- e.g. `/dev/null`, the normal case in a non-interactive
  script or test run -- continues immediately, never hangs) before
  resuming. `DIAMOND_MAX_LOCALS` moved from being compiler.c-local to
  vm.h, shared for the first time: `DIAMOND_OP_DEBUGGER` bakes each
  in-scope local's (name, register) pair into its own bytecode operand
  data at the call site (the compiler is the only place that ever knows
  a register's source variable name), so the opcode's fixed-size decode
  table in `run_chunk` and the compiler's own local-tracking cap have to
  agree on one size. A captured local's value is unwrapped from its
  `DiamondCell` box the same way `GET_CELL` does, checked by the
  *runtime* value kind rather than trusting the compile-time `captured`
  flag passed through. Real blocking-stdin behavior meant its tests live
  in `tests/run.sh` (explicit `</dev/null` control), not
  `tests/cases/*.di` -- the same reason `gets()` itself is only ever
  shadowed, never actually invoked, in that corpus (see `legacy_0333.di`).

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
  and page-cache behavior with the collector's own cost.

  Took that suggestion: `bench/gc_churn` (two short, single-threaded,
  non-networked, non-daemon `.di` scripts, see that directory's own
  README for the full numbers and methodology) gives the clean, direct
  evidence the live `bench/burn_in` pushes above couldn't. Two separate
  findings, disentangled by sweeping live-set size and churn volume
  independently: (1) **per-collection pause cost scales with live-set
  size, not churn** -- holding total allocation volume fixed and growing
  a persistent, continuously-mutated session cache 40x (1,000 → 40,000
  entries) made each individual stop-the-world collection ~7x more
  expensive (15ms → 103ms), because every collection today re-marks and
  re-walks the *entire* live heap regardless of how much of it actually
  changed since the last cycle -- exactly the cost
  `docs/gc-generational-design.md`'s nursery/write-barrier design would
  eliminate for the old, stable part of the heap. (2) **total aggregate
  GC CPU share does not run away with live-set size** in this data -- it
  actually drifts slightly down (40% → 31%) as the live set grows, since
  bigger live sets trigger collections less often. So the case for a
  generational collector here is specifically about bounding individual
  pause length (the metric that shows up directly in tail latency, this
  project's own original `bench/burn_in` motivation), not runaway total-
  CPU cost -- consistent with `bench/burn_in`'s live numbers never
  showing catastrophic growth, just noisy, hard-to-interpret variance. A
  `pure_churn.di` control (same per-iteration allocation shape, zero
  persistent live set) confirms the mechanism directly: it holds a flat
  ~12-13% GC share regardless of iteration count, an order of magnitude
  more collections than the session-cache version but each one nearly
  free, since there's almost nothing live to walk.

  This resolves what the live `bench/burn_in` runs above left "genuinely
  unknown" -- not by explaining that specific run-to-run RSS variance
  (still unresolved, and may not be resolvable without cleaner evidence
  than more live runs can give), but by establishing directly, without
  that confound, that this project's actual generational-GC motivation
  (long-running processes with a large, mostly-stable, continuously-
  mutated live set) is real and measurable: individual pauses that scale
  with live-set size, not with how much garbage churns through it. Still
  not implemented -- `docs/gc-generational-design.md`'s design is the
  next step whenever this is picked up, now backed by direct measurement
  instead of a plausibility argument.

  **A first implementation attempt of that design was built, verified
  correct, and reverted** -- worth recording so the next attempt doesn't
  re-derive the same lesson the hard way. It followed
  `docs/gc-generational-design.md` closely: object header gains `old`/
  `remembered` bits, two intrusive lists (`young_objects`/`old_objects`),
  a small fixed nursery threshold separate from the major collector's
  doubling one, and a write barrier at every mutation site that can store
  a fresh value into an already-existing container (`array_push`,
  `hash_set`, instance field writes, `Cell#value` via `SET_CAPTURE`/
  `SET_CELL`, and the `super()`-into-built-in-Exception-constructor path
  -- the last two are real gaps in the design doc's own mutation-site
  audit, found during implementation). Verification was thorough: clean
  under `DIAMOND_STRESS_MINOR_GC`/`DIAMOND_STRESS_GC` (individually and
  combined) across the full `run_cases` corpus, clean under
  AddressSanitizer with both stress flags, clean under ThreadSanitizer.
  Two real bugs surfaced and got fixed along the way, both in how a major
  collection handled the remembered set: touching a remembered entry's
  `->remembered` bit *after* that same object had already been swept as
  garbage (a straightforward use-after-free, caught by ASan), and --
  after fixing the ordering -- unconditionally clearing the remembered
  set on every major collection at all, which turned out to be a real
  design gap, not just an ordering bug: an old->young edge established
  before a major collection and never written to again has no future
  write-barrier firing to rediscover it, so clearing silently made it
  invisible to every later minor collection. The fix (filter to entries
  whose object is still marked, rather than clearing outright) resolved
  it, and this class of bug is exactly what the design doc's own
  "sharpest risk" section warned about -- silent corruption under load,
  not a crash.

  Correct, but measuring it against `bench/gc_churn`'s own
  `session_churn.di` -- the exact workload this whole investigation was
  built around -- showed a severe regression, not the hoped-for
  improvement: at `live_set_size=20000, iterations=200000`, minor
  collections alone cost **64s** combined (27,901 of them) against **2s**
  for major collections, a large net loss versus the pre-generational
  collector's ~2.5s total GC time for a comparable run. Root cause: the
  write barrier remembers at container granularity, not per-entry, so a
  minor collection has to re-walk *every* entry of a remembered container
  every single time it runs (`mark_remembered_set` recursing through
  `mark_object_children`) -- cheap for a small object with a handful of
  fields (the shape the design doc was implicitly reasoning about), but
  directly proportional to container size for a large, frequently-written
  Hash/Array like `sessions`. Since `sessions` gets touched almost every
  iteration, it stays in the remembered set continuously, and this
  whole-container rescan cost -- which a single-generation collector only
  paid occasionally, at major-collection time -- ended up repeating on
  nearly every one of thousands of minor collections instead. This is a
  genuine limitation of the design as scoped in
  `docs/gc-generational-design.md` (object-granularity write barrier),
  not an implementation mistake; it just took a real measurement against
  the actual motivating workload to surface. A future attempt needs
  field/card-level barrier granularity -- remembering *which entries*
  changed, not just *that* the container changed -- to avoid this;
  `docs/gc-generational-design.md` itself hasn't been updated with this
  finding yet and should be before anyone picks this up again.
