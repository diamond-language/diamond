# Changelog

## Arel 0.14.0

- Extended deterministic inspection to relations, qualified stars, conflict
  attributes, subqueries, joins, CTE declarations, and compound queries.
- Extended structural equality to functions, raw-SQL binds, memberships,
  grouped queries, joins, derived sources, CTEs, and decorated compounds.

## Arel 0.13.0

- Added deterministic non-SQL inspection for core expressions, predicate trees,
  decorators, ranges, memberships, and query summaries.
- Added centralized structural equality for core expressions, predicates,
  decorators, and simple immutable queries while staying within Diamond's
  fixed function-table budget.

## Arel 0.12.0

- Added validated generic function and simple CAST nodes with injection-safety
  coverage, plus structural string concatenation and modulo.
- Added bitwise AND/OR and left/right shift factories with exact bind rendering
  and SQLite execution coverage.

## Arel 0.11.0

- Added named visitor capabilities and early, visitor-specific diagnostics for
  excluded rows, partial conflict targets, upserts, and DEFAULT VALUES.
- Added portable conformance fixtures for SELECT, writes, compounds, and CTEs,
  extension-node metadata, and visitor protocol documentation.

## Arel 0.10.0

- Added explicit visitor selection to SELECT, compound, INSERT, UPDATE, and
  DELETE rendering and execution while preserving SQLite defaults.
- Propagated selected visitors through subqueries, derived sources, compound
  branches, CTE bodies, write expressions, conflicts, and RETURNING, and added
  `Arel.render` as a common entry point.

## Arel 0.9.0

- Added structural, parenthesized addition, subtraction, multiplication, and
  division with bound or expression operands, including excluded-row values.
- Added immutable partial-index conflict targets, structural target columns,
  restricted integer/boolean SQL literals, and SQLite execution coverage.

## Arel 0.8.0

- Added named CTE relation objects, recursive-body construction, declaration by
  relation, and early validation of anchors and self-reference.
- Added structural INSERT expressions, bulk expression bind ordering,
  excluded-row attributes, and INSERT DEFAULT VALUES with RETURNING.

## Arel 0.7.0

- Added ordinary and recursive CTE composition to every write manager, with
  consistent duplicate-name checks and CTE-first bind ordering.
- Preserved nested compound-query grouping, supported compound INSERT sources,
  and rejected compound or INSERT SELECT shapes hidden behind wildcards.

## Arel 0.6.0

- Added multi-row INSERTs with shape validation, INSERT...SELECT, expression
  assignments, SQLite conflict actions, and CTE-backed INSERT statements.
- Added validation and execution coverage for projection counts, conflict
  updates, bind order, and multi-row RETURNING.

Diamond has not made a stable release. This file summarizes completed
capability milestones on the development branch; it does not imply API,
bytecode, or semantic stability.

## Unreleased

### Language

- Built a complete source-to-runtime path: source loading, lexing, direct Pratt
  compilation, register bytecode, VM execution, and managed heap objects.
- Added integers with transparent arbitrary-precision overflow, IEEE-754
  floats, booleans, `nil`, strings, symbols, arrays, hashes, ranges, regexps,
  instances, calendar `Time`, and native resource objects.
- Added expression-valued `if`/`unless`, `elsif`, `while`/`until`, unconditional
  `loop`, value-bearing `break`, `next`, `redo`, postfix conditions, ternaries,
  and equality-based `case`/`when`.
- Added arithmetic, comparisons, `<=>`, modulo, left shift, compound assignment,
  multiple assignment, short-circuit boolean operators, and operator methods.
- Added functions, recursion, closures with shared mutable captures, default and
  keyword arguments, endless definitions, first-class top-level functions, and
  trailing `do |...| ... end` blocks.
- Added classes, constructors, fields, single inheritance, `self`, `super`, class
  variables, singleton methods, runtime method replacement, and method aliases.
- Added modules, nested namespaces, constants, inclusion precedence,
  `module_function`, private visibility, and generated attribute methods.
- Added structural interfaces with inheritance and variance-aware signature
  checks.
- Added optional parameter/return types, unions, nil narrowing, generic
  functions, generic collection contracts, callable signatures, and runtime
  boundary guards.
- Added structured exceptions with typed rescue filters, multiple rescue
  clauses, `else`, `ensure`, `retry`, bare re-raise, causal chains, custom
  payloads, and captured backtraces.
- Added cooperative fibers and isolated-heap OS threads with copied arguments,
  results, and exceptions.
- Added compile-time `require` expansion with relative resolution, extension
  inference, load-once behavior, cycle/limit diagnostics, and imported-source
  mapping.
- Added `ARGV` and `ENV`, `debugger()`/`breakpoint()`, and an accumulating REPL.

### Core library

- Implemented a Diamond-written embedded prelude.
- Added Array and Hash iteration, transformation, lookup, merge, sorting, and
  collection-contract helpers.
- Added Enumerable operations including `select`, `reject`, `map`, `reduce`,
  `count`, predicates, extrema, sorting, grouping, partitioning, slicing,
  zipping, flattening, and tallying.
- Added `Comparable`, integer iteration, `tap`, `dup`, `respond_to?`, numeric
  helpers, `StringBuilder`, string formatting, and extensive String operations.
- Added pure-Diamond JSON parsing/stringification with Unicode escapes.
- Added a Minitest-style library with assertions, exception checks, setup/
  teardown, skipping, summaries, and non-zero failure exit status.

### Native services

- Added stdin/stdout, files, blocking and nonblocking TCP, polling, UDP, signal
  traps, and OpenSSL-backed client/server TLS.
- Added SQLite3 connections with positional binds, typed result rows, execution,
  querying, change counts, insert row IDs, and safe close behavior.
- Added wall-clock and monotonic time APIs, calendar accessors, formatting,
  timezone mode conversion, arithmetic, and comparison.
- Added synchronous subprocess execution with argv isolation and captured
  stdout/stderr/exit status.
- Added regexp construction, matching, capture results, substitution, and scan
  support through the in-repository regex engine.

### Runtime and performance

- Implemented stop-the-world mark/sweep collection, recursive graph tracing,
  explicit frame roots, stress collection, and GC timing counters.
- Replaced linear Hash lookup with an insertion-ordered entry array plus an
  open-addressing bucket table.
- Added class-owned runtime shape chains and polymorphic field caches.
- Added VM-owned polymorphic method caches, monomorphic call-site rewrites,
  deoptimization, invalidation, and dispatch telemetry.
- Added opt-in integer opcode quickening and deoptimization with configurable
  warm-up thresholds.
- Reduced call overhead through register high-water tracking, narrower frame
  initialization, redundant instruction removal, and chunk-copy elimination.
- Replaced quadratic Array joining with a native StringBuilder-backed path.
- Added HTTP, burn-in, dispatch, and GC-churn benchmarks. Direct GC measurement
  established that pause duration grows with the live set.
- Built and reverted a generational collector experiment after measurement did
  not justify its remembered-set/promotion complexity; retained the design and
  failure analysis for future work.
- Ran several interpreter/JIT-adjacent experiments without shipping native-code
  generation.

### Tooling and ecosystem

- Added `facet`, a git-ref package manager with manifests, lockfiles, conflict
  detection, package installation, and package-aware `require` fallback.
- Added HTTP, Rack-style middleware, Gremlin server, and Arel-style SQL query
  builder packages.
- Expanded Arel with quoted expression nodes, grouping/aggregates, structural
  joins, correlated subqueries, recursive CTEs, SQLite set operations, and
  immutable INSERT/UPDATE/DELETE managers with `RETURNING`.
- Added a stdio Language Server with diagnostics, live dependency buffers,
  completion, hover, definitions, document symbols, and workspace symbols.
- Added a VS Code TextMate grammar and hand-written LSP client.
- Added compiler and raw-bytecode libFuzzer targets, including a per-input
  execution watchdog for nonterminating bytecode.
- Added GitLab CI across debug, release, ASan/UBSan, TSan, native VM/fiber,
  package, LSP, REPL, fuzz-smoke, and differential suites.
- Added a shared-process batch test runner that avoids per-case process and
  large-program allocation overhead.

### Self-hosting

- Added `ProgramBuilder`, an internal bridge for Diamond code to construct and
  execute bytecode programs.
- Ported the lexer and compiler to Diamond and added token, diagnostic,
  bytecode, and runtime differential suites against the native frontend.
- Reached self-parse, self-compile, and self-run bootstrap milestones, including
  compiling and executing an independent third program.
- Widened function and register operands to fit compiler-scale programs and
  changed VM frames to use per-function register high-water sizes.
- Added cross-program copying/adoption for structured values and instances
  returned by `ProgramBuilder`.

### Diagnostics and correctness

- Added source spans, caret diagnostics, per-instruction coordinates,
  disassembly, and source-mapped runtime stack traces across imported files.
- Made VM type, arity, bounds, arithmetic, I/O, SQLite, TLS, and stack failures
  rescuable through the ordinary exception hierarchy.
- Fixed compiler memory errors in wide-register type-fact snapshots.
- Fixed multiple GC-rooting defects in regexp capture construction, environment
  population, thread argument copying, and recursive cross-VM value copying.
- Fixed stale/dangling native frame roots during fiber execution and nested
  resumption.
- Fixed closure boxing and register-aliasing bugs across branches, stored
  callbacks, indexed assignment receivers, and trailing block capture.
- Fixed call-depth limits that could otherwise allow the native C stack to
  overflow before Diamond raised `SystemStackError`.
- Fixed loader edge cases for CRLF input, nested imports, EOF diagnostics,
  directories, unreadable files, source-map bounds, and runtime trace mapping.
- Fixed method-cache invalidation, quickening/deoptimization gaps, type
  confusion in destructuring checks, TLS partial-I/O handling, and thread/GC
  lifecycle races found by focused tests and sanitizers.
