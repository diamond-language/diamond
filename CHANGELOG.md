# Changelog

Diamond is currently pre-release. This file records user-visible capability
milestones rather than every implementation step; the Git history remains the
authoritative fine-grained record.

## Unreleased

### Diagnostics

- Compiler diagnostics now own their formatted message text, so messages remain
  valid after compiler teardown and struct copies. Non-exhaustive `case` errors
  use this to name each missing union member or sealed subclass.

### Tooling

- Added a bare `clear` REPL command (irb/pry/python-REPL-style, same
  convention as the existing bare `exit`/`quit`): wipes the visible
  terminal screen and redraws a fresh `{> ` prompt without touching the
  running session (accumulated declarations, history, etc.). Recognized
  only when a real terminal is attached; a piped/non-interactive session
  still consumes the line as a no-op rather than trying to compile it as
  Diamond source.

### Performance

- JIT statically typed String `length`, `index_of`, and `ord`, plus Array/Hash
  `length`, `key_at`, and `value_at` reads on x86-64.
- JIT statically typed `String#slice` with GC-safe allocation and direct error
  propagation on x86-64.

- JIT method-result chaining now covers an ivar-loaded inner receiver:
  `result = @factory.make(); result.use()`. The discovery pass preserves
  its final, whole-class ivar type facts for the real code-generation pass,
  so this remains independent of method and reopening order and rejects a
  field with any untyped or conflicting write. The focused release benchmark
  is about 49% faster (~8.3s to ~4.3s). See JIT Phase 16.
- JIT now also compiles `.method()` on a local whose only assignment is
  an ivar read (`x = @field; ...; x.method()`), when the field's
  compile-time type is a single concrete class -- closing the roadmap's
  named "an ivar load" gap alongside Phase 10's own `.new()`-result case.
  Unlike a method call's own return type, a field's type can't be
  invalidated by `redefine_method` at runtime, so this needed a new
  per-function snapshot of the class's own field-type table rather than
  the harder redefine-safety question a method call's return value would
  raise. Fixed two related, pre-existing gaps in that field-type table
  along the way: a field whose only assignment came from an
  attr_accessor-generated writer, or a struct-declared field, both used
  to look "never assigned" instead of contributing a real known-class (or
  correctly-unknown) fact. See docs/internal/jit-design.md's "Phase 11".
- JIT now also compiles `.method()` on a local whose only assignment is a
  method call resolved to a target with a declared, single-concrete-class
  return type (`x = obj.method(); ...; x.other()`), when `obj` is itself a
  typed parameter or a freshly-`new`'d local -- closing the roadmap's own
  last-named `INVOKE`-receiver gap for that slice. Unlike Phase 10/11's own
  facts, a method's return type can go stale at runtime via
  `redefine_method`; a new whole-program flag (set the moment
  `redefine_method` is called anywhere, on any class) disables this
  optimization program-wide when that happens, rather than risking a wrong
  dispatch. `self` and an ivar load are not yet valid receivers for the
  *inner* call specifically (neither currently feeds the compiler's own
  static type tracking this depends on) -- a real, separately fixable gap,
  not something this change silently drops. See docs/internal/
  jit-design.md's "Phase 12".
- `self.method()`'s own result now also chains the same way (`x =
  self.make(); ...; x.other()`), closing the `self` half of the gap just
  above -- `self`'s own register now feeds the compiler's real type
  tracking the same way a typed parameter's already did, needing no
  further JIT changes at all. An ivar load still doesn't chain. Measured
  no improvement on skindicate's own real ORM hot path despite it being
  exactly `self`-shaped: traced to Arel's own hot accessor methods
  (`Query#projections`, `BinaryNode#left`, ...) having no explicit
  return-type annotation at all, which this mechanism never trusts
  regardless of receiver kind -- a separate, application-level gap, not
  a compiler one. See docs/internal/jit-design.md's Phase 12 addendum.
- The JIT now compiles `is` type checks (`DIAMOND_OP_IS_TYPE`) at all --
  previously any function containing one, however simple, failed to
  JIT-compile at all. Also fixes a related bug where an unrelated `is`
  check anywhere in a function silently disabled the `self`/typed-
  parameter/`.new()`-result method-chaining optimizations above for
  *every other* register in that same function. Narrowing a declared-
  union parameter with `is` still doesn't make a *chained call on it*
  JIT-eligible (`if x is Derived; x.helper().double(); end`) -- that
  remains open, and needs a different, position-sensitive mechanism
  from anything built so far. ~39% faster on a new
  `bench/jit_is_type.di`. See docs/internal/jit-design.md's "Phase 14".
- JIT now also compiles a chained method call on a receiver narrowed by
  `is`, even from a declared-union parameter that's never provably one
  class anywhere else in the function (`if x is Derived; y = x.helper();
  y.double(); end`) -- closing the gap Phase 14 explicitly left open.
  Position-sensitive (a new per-call-site fact, not a per-register one),
  deliberately not the simpler per-register approach considered first,
  which was rejected because the real motivating shape (Arel's own
  `render_expression`) narrows the same register to different classes at
  different call sites -- confirmed via a stash-based A/B that the
  simpler design would have missed exactly that case. ~38% faster on
  a new `bench/jit_is_narrowed_invoke.di`. See docs/internal/
  jit-design.md's "Phase 15".

## 0.6.0

### Tooling

- Added `textDocument/formatting` to the Language Server: normalizes
  every physical line's own leading indentation to a consistent
  2-space-per-level and trims trailing whitespace, tracking nesting
  from the same token stream every other handler here already uses --
  deliberately not a full AST-based pretty-printer (the native
  compiler retains no AST at all). Verified against every real
  (non-test-fixture) `*.di` file in this repository (309 files, zero
  bailouts) before trusting it; found and fixed five real grammar
  gaps along the way (an endless `def name(...) -> Type = expr` whose
  return-type scan could run straight past the header looking for any
  `=` anywhere later in the file; `loop`/`while`/`until`'s own
  optional trailing `do` double-counted as a second nested block
  instead of the same one; the `closure name(...) ... end` keyword
  missing from block-tracking entirely; `if`/`unless`/`while`/`until`
  used as a value inside a call argument misclassified as a postfix
  modifier; and an interface's own signature-only `def` expecting a
  matching `end` that never comes). Returns `null` rather than a
  guess when it isn't confident, never no edits vs. a guessed one.
  See docs/lsp.md.

### Performance

- Fixed a real, already-deployed crash under `DIAMOND_JIT=1`: a compiled
  function combining a call-capable opcode that doesn't need a
  `DiamondFrame` (`EQUAL`'s own general case, or any arithmetic/
  comparison opcode) with a *later* opcode that genuinely needs to
  propagate a real error (e.g. writing to a frozen instance) could
  corrupt the VM's own internal frame chain and eventually segfault --
  traced to `emit_epilogue_propagate` unconditionally popping a
  `DiamondFrame` that this specific call shape never actually pushed.
  Reproduces back to `EQUAL`'s own general case (2026-09-13); unrelated
  to and predates the arithmetic work below. See docs/internal/
  jit-design.md's "Phase 5".
- JIT (`DIAMOND_JIT=1`) no longer rejects compiling `ADD_INT`/
  `SUBTRACT_INT`/`MULTIPLY_INT`/`DIVIDE_INT`/`LESS_INT` once an earlier
  call-capable opcode has already run in the same function -- their own
  overflow/division-by-zero/non-`Int`-operand edge cases now resume via
  a dedicated trampoline instead of needing to discard and retry the
  whole function. See docs/internal/jit-design.md's "Phase 3".
- JIT now also compiles generic (non-`_INT`) `ADD`/`SUBTRACT`/
  `MULTIPLY`/`DIVIDE`/`LESS`/`LESS_EQUAL`/`GREATER`/`GREATER_EQUAL` --
  previously any value the compiler couldn't statically prove `Int` (an
  untyped parameter, a Hash/Array element) used in arithmetic anywhere
  disabled the JIT for its entire containing function. Real, measured
  wins: `bench/int_arithmetic_dynamic.di` ~2.1x faster JIT'd,
  `bench/hash_ops.di` ~1.5x. See docs/internal/jit-design.md's "Phase 5".
- JIT now compiles `.dup()`/`.freeze()`/`.frozen?()` -- none of the three
  can invoke arbitrary user code, so this needed no new safety
  mechanism -- on `Array`/`Hash`/`String`/`Symbol`/primitive receivers,
  and (a follow-on) on an Instance receiver too as long as its class
  doesn't override that method itself. Closes the last of three gaps
  that had kept skindicate's own `Model#initialize` (as of its current,
  `.dup()`-based shape) permanently JIT-ineligible. See docs/internal/
  jit-design.md's "Phase 4" and "Phase 6".
- JIT now compiles `self.method()` dynamic dispatch for any method name
  (not just `dup`/`freeze`/`frozen?`), including a real override -- until
  now, any dynamic method call anywhere in a function's body, including
  a plain `self.other_method()`, bailed that whole function out of JIT
  eligibility. Real, measured win: ~35% faster on a `self.method()`-in-
  a-loop shape (`bench/jit_invoke_self.di`). Any receiver other than
  `self` remains unsupported. See docs/internal/jit-design.md's "Phase 7".
- JIT now also compiles `other.method()` dynamic dispatch when `other` is
  a declared parameter with a single, explicitly-annotated concrete class
  type that the function body never reassigns -- a real, provably-safe
  slice of non-`self` dispatch, closing part of the one gap Phase 7 left
  open. Real, measured win: ~38% faster on a typed-parameter-method-in-
  a-loop shape (`bench/jit_invoke_typed_param.di`). A union/nilable type,
  a reassigned parameter, an untyped parameter, and any non-parameter
  receiver (a local, a prior call's return value) remain unsupported. See
  docs/internal/jit-design.md's "Phase 9".
- JIT now compiles `DIAMOND_OP_NEW` (`SomeClass.new(...)`), and a later
  `.method()` call on the freshly-constructed result when assigned to a
  local that's never reassigned -- until now, any function containing a
  `.new()` call bailed out of JIT eligibility entirely, regardless of what
  happened to the constructed value afterward. Closes another real slice
  of non-`self` dispatch alongside Phase 9's own typed-parameter case.
  Keyword/spread construction (`SomeClass.new(field: value)`) remains
  unsupported. See docs/internal/jit-design.md's "Phase 10".
- `Arel::Query#to_sql` no longer allocates a fresh `SQLiteVisitor` on
  every call when the caller doesn't pass its own visitor (the common
  case for every SQLite-backed app) -- `SQLiteVisitor` carries no
  instance state of its own, and `Visitor#render`'s only stateful field
  is already saved/restored around every call so the same instance can
  safely render nested subqueries recursively, so a lazily-memoized
  per-thread default instance is exactly as safe as that existing
  recursion. Verified correct (full suite, plus a direct two-query
  reuse check) and measured on skindicate.dia's own `/` route via a
  controlled A/B: within noise (~27.0ms -> ~26.7ms) on that specific
  workload -- a real, legitimate allocation removed, but request time
  there is dominated by other costs, so don't expect this alone to move
  a request-latency number.
- `Visitor#render` no longer calls `.map()`/`.each()` at all for an empty
  `GROUP BY`/`HAVING`/`ORDER BY`/`JOIN` clause (the common case for a
  simple query) -- previously always built a closure and dispatched the
  iteration method even with zero elements to visit. Measured via an
  isolated microbenchmark (`bench/arel_render.di`, 20000 renders of a
  two-predicate query, no DB/HTTP/template noise): a real but modest
  ~1-2% win, on top of the (separately negligible) `SQLiteVisitor`-reuse
  fix above -- confirms this workload's cost is genuinely spread across
  many small interpreted calls, not concentrated in any one fixable spot.

### Language

- Fixed `puts`/string interpolation/the CLI's own top-level-result
  auto-print misreporting any native resource value (`Tensor`, `Channel`,
  `Supervisor`, `Regexp`, `Fiber`, `File`, `Thread`, `SQLite3`, ...) as
  `#<Closure>` -- correct only for an actual `Closure`, silently wrong
  for everything else added since. Two independent stringification
  functions (`src/vm.c`'s `builder_format_value`, `src/value.c`'s
  `diamond_value_fprint`) each had their own hardcoded `"#<Closure>"`
  fallback for any object kind neither one explicitly handled; both now
  share one canonical per-kind name table
  (`diamond_format_value_type`, exported from `src/vm.c` via `vm.h`)
  instead of two independently hand-maintained copies. Found writing a
  `Tensor#matmul` benchmark. `Time` needed a second fix beyond the
  shared name table: the CLI's own auto-print had no `Time` case at
  all (unlike `puts`/interpolation, which already formatted it
  correctly), so it printed the generic `#<Time>` placeholder instead
  of a real date -- `diamond_format_time_default` (`src/vm.c`) is now
  exported too, so both paths share the exact same formatting instead
  of one of them lacking it.

## 0.5.2

### Documentation

- Rewrote README.md into a compact, comparison-focused introduction (what's
  familiar coming from Ruby, what actually differs) instead of an exhaustive
  feature/build-matrix dump. The system-dependency install commands,
  loopback-network and sanitizer/ptrace test notes, and self-hosting corpus
  note it previously carried moved into CONTRIBUTING.md's own "Build and
  test" section instead -- CONTRIBUTING.md already pointed contributors at
  README.md for exactly this detail, so nothing here is lost, just
  relocated to where a contributor (rather than a first-time reader) looks
  for it.

## 0.5.1

### Tooling

- Added `textDocument/references` to the Language Server: finds every
  workspace occurrence of the top-level function, class, module, or
  interface name under the cursor -- the same globally-unambiguous
  declaration kinds hover/definition/documentSymbol already single out
  -- by walking every `*.di` file under the workspace root the same way
  `workspace/symbol` does and tokenizing each compiled file's own buffer
  for the name in a resolvable position (a call/access site, a type
  position, or a class/module/interface declaration header). A candidate
  is dropped if a lexical local of the same name is in scope there,
  ruling out a keyword-argument label, a hash key, or a shadowing local.
  Deliberately name-based, not a full alias-aware resolver -- a bare-name
  reference with none of those adjacent shapes (a class passed as a
  first-class value, say) isn't found, a known, deliberate
  under-approximation. See docs/lsp.md.
- Added `textDocument/rename`, sharing `textDocument/references`'s own
  workspace scan directly rather than duplicating it: the same search,
  built into a `WorkspaceEdit` instead of a `Location[]`. The new name
  must lex as exactly one identifier token consuming the whole string
  (rejects empty, a keyword, a qualified `A::B` name, or embedded
  whitespace), since accepting anything else would hand back an edit
  guaranteed not to recompile. No collision detection against an
  existing same-named symbol at the target scope -- the same
  deliberately name-based, conservative-scope cut `references` itself
  already makes. See docs/lsp.md.

## 0.5.0

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

- Re-ran the focused ThreadSanitizer concurrency suite across `Thread`,
  `Channel`, `Supervisor`, and representative `Fiber` cases: all 35 cases
  passed with no data races reported.
- Extended per-case minor-GC stress coverage to Channel and Supervisor
  producer/consumer, restart, and nested-tree cases. The batch runner now
  isolates `DIAMOND_STRESS_MINOR_GC` between cases just like major-GC stress.
- Audited TCP, TLS, and `Process.spawn` timeout/error cleanup paths, including
  descriptor closure after failed setup and rooted TLS-handle cleanup after a
  handshake failure; existing timeout and failure cases passed without leaks or
  double-close symptoms.
- Revalidated the native and LSP suites with the locally installed Fedora
  Clang 22 toolchain: 1543 language cases and 277 LSP cases passed.
- Added combined major/minor-GC stress coverage for SQLite prepared-statement
  reuse/closure and statements that outlive their closed connection; both
  ownership paths continue to pass.
- Revalidated live database integration against throwaway PostgreSQL 16,
  MariaDB 11, and MySQL 8 containers: each Arel dialect suite passed all
  12 tests with no cleanup or driver error failures.
- The three live-driver Arel scripts now run with both major and minor GC
  stress enabled; PostgreSQL, MariaDB, and MySQL passed 36/36 tests under
  forced collection.
- SQLite invalid-query, multi-statement-guard, parameter-mismatch, and
  closed-connection/statement errors now run under combined major/minor-GC
  stress as well.
- Added explicit invalid-query and use-after-close cleanup assertions to the
  live PostgreSQL, MariaDB, and MySQL suites; all three now pass 13/13 under
  combined major/minor-GC stress.

### Tooling

- Clarified the remaining portability milestone: Linux arm64 needs a real
  build-and-test runner before Diamond can claim non-x86_64 coverage; a
  compiler-only cross-build is not sufficient for fibers, atomics, loading,
  and subprocess behavior.
- Added a deliberately narrow native Ubuntu arm64 CI job (`test-arm64`) that
  runs the baseline `make test` corpus before sanitizer, LSP, or package
  coverage is expanded.
- Made `DIAMOND_JIT` a safe no-op outside glibc x86-64; the portable
  interpreter now remains in control instead of attempting an unvalidated
  machine-code/ABI combination. The arm64, musl, and macOS CI baselines
  exclude only the JIT-specific cases.
- Restored self-hosted lexer parity for the `sealed` and `struct` keywords;
  the native/self-hosted lexer differential suite now covers all 1,438 lexer
  cases without unknown-token fallbacks, and the parser differential suite
  remains green at 253 positive plus 126 negative cases.
- Extended receiver-aware generic call chains to explicit union bindings:
  `identity[Pet | Leaf](value).method()` now exposes both possible receiver
  classes to hover, definition, and completion, including after indexing a
  nested shape such as `identity[Array[Pet | Leaf]](values)[0]`.
- Generic receiver inference now retains every class in a union-valued
  argument, both for a bare `T` and through `Array[T]`, rather than abandoning
  the call chain unless the argument resolved to exactly one class.
- Generic receiver inference now also descends through both parameters of a
  `Hash[K, V]` shape, so a function accepting `Hash[String, T]` can recover a
  `Pet | Leaf` result from a `Hash[String, Pet | Leaf]` argument. Direct hash
  indexing remains unresolved because its runtime result is nullable.
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
  into/out yet -- see docs/debugging.md for the full contract and
  limitations, and docs/roadmap.md's "Step debugger v2" for what's next.
  `editors/vscode` wires this up as a VS Code debugger via
  `diamond.debugAdapterPath`.
- Added live breakpoints: a `setBreakpoints` request no longer requires
  restarting the debuggee, whether it arrives before launch, while
  running, or while already stopped at a different line. A new
  `DIAMOND_OP_BREAKPOINT_CHECK`, emitted at *every* statement whenever a
  debug session is compiling at all (regardless of which lines, if any,
  were selected beforehand), checks a runtime-mutable armed-line set
  (`DiamondVm.debug_active_lines`) instead of always pausing the way the
  compile-time-selected `DIAMOND_OP_DEBUGGER` did -- `diamond-dap` pushes
  the complete current set over the existing control channel on every
  `setBreakpoints` call once the debuggee is running
  (`send_live_breakpoints`, `dap/main.c`). Deliberately confined to this
  one new opcode's own case rather than `run_chunk`'s shared dispatch
  header, so an ordinary (non-debug) run has none of this instrumentation
  and pays nothing for the feature existing.
- Added real stepping: `next` (step over), `stepIn`, and `stepOut`, all
  reusing the exact same `DIAMOND_OP_BREAKPOINT_CHECK` live breakpoints
  already install everywhere -- no new bytecode or compile-time mechanism
  needed. Sending one while stopped arms a runtime step mode with the
  current pause's own already-tracked call depth as a target; the next
  checkpoint hit satisfying that mode's own depth comparison pauses
  (`"reason":"step"` in the resulting DAP `stopped` event, alongside the
  existing `"reason":"breakpoint"`) -- a real armed breakpoint line
  always still wins regardless of any pending step. See
  docs/debugging.md's "Stepping" section for the exact depth semantics
  and one accepted edge case around self-recursive tail calls.
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
- `diamond build` now preserves arbitrarily long output paths and `--cc`
  values when passing them to `make`; its former fixed argument buffers could
  silently truncate either value and build the wrong target or invoke the
  wrong compiler.

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

### Language

- Added exhaustiveness checking for `case`/`when`: a `case` whose subject has
  a known union type made entirely of `nil` and/or user classes now requires
  either an `else` or an unguarded `when` naming every member, or it's a
  compile error instead of silently returning `nil` from the uncovered path.
  Deliberately conservative -- a single non-union type naming an ordinary
  (non-sealed -- see below) class, or a union containing any native scalar/
  container type, interface, or generic type variable, is left exactly as
  unchecked as before (no `when` syntax can prove a whole native type is
  covered today); a bare class-name/`nil` scalar `when` value counts as
  coverage, and so does a single, top-level, empty `Circle{}` object
  pattern (added later -- see below), but never a non-empty or nested
  structural pattern, an Array/Hash pattern, several comma-separated
  structural alternatives in one `when`, a guarded `when ... if` clause,
  or a superclass covering a subclass union member. See docs/core-syntax.md's
  "Exhaustiveness checking" section.
- Added `sealed class Name ... end`: an opt-in author promise that a class
  hierarchy is closed, not an enforcement mechanism against some external
  boundary (Diamond has no per-file/module compile boundary that survives
  into the compiler at all -- see docs/classes-and-modules.md's "Sealed
  classes" section for the full architectural reasoning). Two effects:
  `Shape.new(...)` becomes a compile error (only a subclass can be
  constructed), and a `case` subject whose plain (non-union) type names a
  sealed class becomes eligible for the exhaustiveness checking above, over
  that class's own direct subclasses -- a zero- or more-than-8-subclass
  sealed class stays unchecked either way, matching the same member-count
  ceiling every union already has.
- Fixed a real exhaustiveness-checking false positive: a single, top-level,
  empty `Circle{}` object pattern (no reader fields, no `,`-joined
  alternatives) previously never counted as covering `Circle`, so a `case`
  whose every arm used one was wrongly rejected as non-exhaustive even
  though it demonstrably covered every member -- forcing a spurious `else`
  or a drop back to a bare `when Circle` just to satisfy the checker. Now
  credited the same way a bare `when Circle` already was, for both an
  explicit union and a sealed hierarchy; a non-empty or nested pattern
  still correctly doesn't count, since it only matches a subset of the
  type.
- Added self-recursive tail-call optimization: a function's own tail call
  to itself (the entire value of a `return`, or a body's own trailing
  expression, nothing done to the result afterward) now runs in constant
  native stack space instead of the ordinary 95-level call-depth limit.
  Transparent -- never changes what a program computes, only how deep a
  qualifying recursive shape can go before hitting that limit. Excludes,
  falling back to ordinary bounded recursion rather than erring: non-tail
  self-recursion, a tail call inside `begin`/`rescue`/`ensure`, mutual
  recursion (a different function, even one that tail-calls back), a
  generic function's own self-call, and a variadic function's own
  self-call. See docs/callables.md's "Tail-call optimization" section,
  including the one real observable consequence: a function that
  recurses forever with no base case, written as a qualifying tail call,
  now loops forever instead of eventually raising `SystemStackError`.
- Added `freeze()`/`frozen?()` for `Array`, `Hash`, and `Instance`: once
  frozen, every native mutation those three kinds support (`Array#push`/
  `#pop`, `[]=` on an `Array` or `Hash`, an instance variable write, and
  anything -- like `delete_at` -- built from those in the standard
  library) raises a rescuable `FrozenError` instead of proceeding.
  `freeze()` returns the receiver (chainable); for a primitive or an
  already-immutable `String`/`Symbol`, both mirror `dup`'s own existing
  "already immutable" precedent (a harmless no-op, always `true`) rather
  than an "undefined method" error. `dup` never carries frozen status to
  the copy (Ruby's own `dup`-vs-`clone` distinction), and freezing is
  shallow -- a frozen value's own fields/elements are unaffected. Fixed
  a real, previously-latent bug found while adding this:
  `DIAMOND_OP_SET_IVAR`'s interpreter case and the JIT's own
  `diamond_jit_set_ivar` trampoline were two independent, byte-for-byte
  duplicated implementations, which would have made freezing silently
  not apply to instance-variable writes in JIT-compiled methods; the
  interpreter's own case is now a thin wrapper calling the shared
  trampoline, matching the same fix already applied to `INDEX_SET` in an
  earlier JIT phase. See docs/classes-and-modules.md's "tap / dup /
  freeze / frozen? / respond_to? / public_send" section.
- Added `struct Name(field: Type, ...) ... end`: a compile-time-only data
  class declaration (no runtime class synthesis) that generates
  `initialize`, one reader per field, `==`, and `to_s` automatically --
  `struct Point(x: Int, y: Int)` gives `Point.new(1, 2)` (or the keyword
  form `Point.new(x: 1, y: 2)`), `.x()`/`.y()`, field-wise `==` (`false`,
  never a raised error, against an unrelated type), and
  `"Point(x: 1, y: 2)"` from `.to_s()`. Deliberately narrow for this
  first pass: no superclass, no reopening. Composes normally with
  everything else -- a struct's class can appear in a `Type | Type`
  union or participate in exhaustiveness checking like any other class,
  `.freeze()`/`.frozen?()` work on an instance the ordinary way, and a
  field's declared type may refer to the struct's own name (`struct
  Node(value: Int, rest: Node | Nil)`). See docs/classes-and-modules.md's
  "`struct` declarations" section.
- A struct's body can now supplement its generated methods with ordinary
  `def`/`attr`/`include`/visibility/`alias_method`/`delegate` lines
  between the field list and `end` -- `compile_class`'s own body-parsing
  loop is now shared (`compile_class_body`, `src/compiler.c`) between
  `class` and `struct`. A hand-written member can only add, not
  override: a name collision with a generated reader/`initialize`/`==`/
  `to_s` fails with the same "duplicate or excessive method definition"/
  "attribute method is already defined" error an ordinary `class`
  already gives for defining a method twice, with no struct-specific
  handling needed. See docs/classes-and-modules.md's "`struct`
  declarations" section.

### Packages

- Added `facet init [name]` (writes a fresh `diamond.cut`, refusing to overwrite
  an existing one) and `facet add <name> --git <url> (--tag <ref> | --branch <ref>
  | --commit <ref> | --version <constraint>)` (appends a dependency and
  rewrites the manifest) -- `diamond.cut` no longer has to be hand-edited from
  scratch, though `facet add` still regenerates the whole file rather than
  patching it in place, so hand-added comments or unusual formatting don't
  survive it. See docs/packages.md's "`facet init` and `facet add`" section.
- Added real backtracking to `facet`'s dependency resolver, closing the
  one gap 0.3's own semver resolver explicitly deferred: a version-
  constrained dependency that resolves before every requester's own
  constraint on it is known used to hard-error unconditionally the
  moment a later, tighter constraint turned up, even when a different,
  still-available tag would have satisfied everyone. `resolve_full_graph`
  now retries the whole graph walk (wiping the ephemeral scratch clones
  each time) when that happens, carrying forward the full intersected
  constraint history for every version-constrained name across attempts
  so the next attempt resolves it correctly the first time. A genuinely
  disjoint pair of constraints (no tag could ever satisfy both) still
  hard-errors immediately, with no restart wasted. See docs/packages.md's
  "Real backtracking" section.

### Performance

- Added transparent bytecode caching: `diamond script.di` now caches the compiled
  program in a sibling `<script>.dic` file, so an unchanged second run skips
  compilation entirely (a real ~13x startup win measured against skindicate.dia's own
  substantial multi-file `app.di`). Keyed on a SHA-256 hash of the fully `require`-
  expanded source (so editing a required library file invalidates the cache even
  when the entry file itself is untouched) and validated against a build fingerprint
  covering exactly what the underlying serialization format depends on
  (`sizeof(DiamondFunction)`/`DiamondClass`/etc., `DIAMOND_OP_COUNT`, ...) -- a stale,
  incompatible, or hand-corrupted cache file is always just a cache miss, never a
  crash. On by default for the ordinary CLI; `DIAMOND_NO_CACHE=1` opts out,
  `DIAMOND_TRACE_CACHE=1` reports hit/miss/write. `-e`, the REPL, a step-debugger
  session, `diamond build`, and the test suite's own batch corpus runner never use it
  -- see docs/caching.md for the full contract and docs/roadmap.md's "Bytecode
  caching: what's next, if anything" for what's deferred.
- A comparison against a native `.length()`/`.to_i()`/`.ord()`/etc. call
  (`while i < s.length()`, `s` a `String`, `Array`, or `Hash`) now
  compiles straight to `LESS_INT` (and siblings) from its very first
  execution under the plain interpreter, with no dependence on
  `DIAMOND_QUICKEN` ever observing enough int-int comparisons to rewrite
  it in place -- the receiver's call result now publishes its known,
  fixed scalar return type at compile time (reusing `DIAMOND_NATIVE_
  METHODS`, the same table already used for structural interface
  conformance checking) the same way `.keys()`/`.values()` already did
  for their own container types. Fixed same-day to actually cover a
  plain `String` receiver, not just `Array`/`Hash`: the first version
  only fired for a receiver with a *registered type set*, which `Array`/
  `Hash` locals always have (needed for element-type tracking
  regardless) but a bare `String` local never does -- confirmed directly
  via `--dump-bytecode` that `s.length()` on a plain String still
  compiled to generic `LESS` before this correction. An interpreter-level
  improvement only -- confirmed this does **not** move JIT (`DIAMOND_JIT=1`)
  coverage any closer to compiling a method that reads an instance
  variable or calls a native collection method; both remain entirely
  unsupported by the JIT today, independently of this change. See
  docs/internal/jit-design.md's "Phase 2f" and docs/roadmap.md's
  "Native-code execution" for the full, corrected picture of what's
  still needed there.
- JIT (`DIAMOND_JIT=1`) now supports `DIAMOND_OP_GET_IVAR` (reading one
  of an instance's own fields), via a new `diamond_jit_get_ivar`
  trampoline mirroring the existing `SET_IVAR` one exactly. A method
  that only reads/writes its own instance variables plus does int
  arithmetic is now fully JIT-eligible where it previously bailed
  unconditionally -- a real, if narrow, class of methods (accessors,
  counters, accumulators). Does not, by itself, make any method that
  also calls another method or does Hash/Array indexing (such as
  skindicate's own `Model#initialize`) JIT-eligible -- see
  docs/internal/jit-design.md's "Phase 2g" and docs/roadmap.md's
  "Native-code execution" for the two gaps that remain.
- `div` templates' HTML-escaping (`Div.escape_html` and the equivalent
  helper inlined into every compiled template) now skips its
  char-by-char `StringBuilder` loop entirely for the common case of a
  string containing none of `& < > " '` -- a cheap upfront native
  `.index_of()` scan returns the input unchanged instead of paying one
  single-character String allocation per byte for output that ends up
  byte-identical to the input anyway. Found via profiling skindicate.dia's
  own root page locally: rendering a 30-card grid dropped from ~55ms to
  ~43ms (~22% faster), cutting total request time from ~86-98ms to
  ~72ms -- escaping alone is ~11x faster in isolation
  (a targeted microbenchmark went from ~7.4ms to ~0.7ms for the same
  escape workload), though grid rendering does more than just escaping,
  which is why the end-to-end win is smaller than that. No behavior
  change for any input, including the rare case where escaping actually
  is needed (unchanged, still the original char-by-char loop).
- `ActiveRecord::Model#initialize` now copies its incoming attributes
  via the native `Hash#dup` instead of a manual `.keys()` + indexed
  loop -- both produce the same independent, mutable copy, but `#dup`
  measured ~16-24x faster across realistic row shapes (7-16 columns).
  Every row any query returns constructs one `Model` instance, so this
  runs on every row of every query result across every ActiveRecord-based
  app (skindicate.dia, project_board, pheint.dia). Found continuing the
  same skindicate.dia profiling pass as the `div` escaping fix above,
  after ruling out template rendering and `StringBuilder` as the
  remaining cost (both measured negligible at this scale) and tracing
  the real remaining time into ActiveRecord's row-to-object hydration:
  cut `SkinsController#index`'s own measured cost from ~43ms to ~26ms
  (~41% faster), and the real end-to-end root-page request from ~72ms
  to ~52-58ms. No behavior change -- `dirty_attributes.di` and every
  other `@attributes` reader/writer in `model.di` work identically
  against either copy.
- Added native in-place elementwise/shape `Tensor` mutators and their
  backward passes (`#add!`/`#scale!`/`#add_bias!`/`#row_softmax!`/
  `#gelu!`/`#column_sums`/`#columns`/`#add_columns!`/`#clone`/
  `#softmax_backward`/`#gelu_backward`/`#layernorm_forward`/
  `#layernorm_backward`), driven by a real training loop's own measured
  bottleneck (see docs/collections.md's `Tensor` paragraph and
  `examples/transformer` below).

### Applications and examples

- Added `examples/transformer`, a from-scratch GPT-style decoder-only
  transformer on `Tensor`: multi-head causal self-attention, LayerNorm,
  GELU feed-forward, reverse-mode autograd checked against numerical
  gradients (`gradcheck.di`), SGD training, JSON checkpointing, a
  byte-level tokenizer/corpus pipeline for real text data, and greedy
  generation. Deliberately narrow (see its own README's "Scope" section:
  plain SGD, no batching, `Tensor`'s own 2D-only shape worked around via
  column-slicing rather than a real reshape) -- it exists to give
  `Tensor`'s "before committing to a fuller API surface" question
  (docs/collections.md) a real answer to point at.

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
