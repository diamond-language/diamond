# Changelog

## Applications

- Added `applications/pheint.dia`, a runnable product-oriented GraphQL-only
  Diamond API for a separate React frontend, with Rails-like source layout,
  development/test/production configuration, Gremlin serving, Rack
  middleware, Dials routes, strict NDJSON request logging, correlation IDs,
  sub-millisecond timing, JSON service/health endpoints, and direct-dispatch
  smoke coverage.
- Added `make test-pheint-application`.
- Added environment-isolated pheint.dia databases and instrumented
  ActiveRecord models for accounts, one-to-one players with unique handles,
  and expiring server-side authentication sessions.
- Added GraphQL `signUp`, `signIn`, `signOut`, and `me` operations with
  normalized email/handles, transactional account/player/session creation,
  bcrypt password verification, 256-bit bearer tokens, 30-day expiry,
  constant-work unknown-account login rejection, and correlated audit logs.
- Added pheint.dia game, leaderboard, and score models with explicit ownership
  and player associations, cascading foreign keys, validation, a database
  uniqueness constraint on `[leaderboard_id, player_id]`, and representative
  seeded game/leaderboard/score data.
- Exposed the seeded game hierarchy through a public GraphQL `games` query,
  using GraphSQL lookahead projection and recursive batch preloading for owner,
  player, leaderboard, and score associations.

## Language

- Added forward and mutually recursive top-level function resolution, including
  generic/keyword calls, callable references, and `require`-expanded files.
- Synced the self-hosted lexer and its differential token dumper with the
  native `closure` and `delegate` keywords.
- Corrected self-hosted parser package fixtures to exercise `require_cut`
  instead of silently comparing matching failed legacy `require` calls.
- Removed an obsolete self-hosted error fixture that still expected singleton
  method references to be rejected as missing-parenthesis calls.
- Added OpenSSL-backed `Digest.sha256` and `HMAC.sha256` over raw String bytes,
  returning deterministic lowercase hexadecimal digests.
- Made LSP receiver completion, hover, and go-to-definition position-sensitive
  across local reassignment, including transitions from explicit unions to a
  concrete assigned class.
- Added conservative instance-variable receiver tooling across class methods
  when every assignment to the field agrees on one concrete class.
- Added recursive LSP resolution through explicitly typed call results,
  including constructors, top-level and singleton factories, instance methods,
  and class-union returns, for completion, hover, and go-to-definition.
- Added conservative union metadata at `if`/`unless` and ternary control-flow
  joins, enabling receiver tooling after branching assignments and removing
  redundant compatible return checks without changing dynamic failure behavior.
- Added Range, Regexp, class/subclass, and custom-equality pattern matching to
  `case`/`when`, plus conservative union joins for case results and locals.
- Added conservative type joins across `while`, `until`, and unconditional
  loop exits, including local assignments and valued `break` expressions.
- Added nested, bracketed Array destructuring assignment with strict type and
  arity checks at every level and local, instance, or class-variable leaves.
- Added exact nested Array patterns to `case`/`when`, with lowercase element
  bindings, `_` wildcards, recursive value patterns, and failure-atomic binding.
- Added trailing Array rest patterns (`[head, *tail]`) to destructuring
  assignment and `case`, including nested patterns and `*_` remainder discard.
- Added `^local` pins to nested `case` Array patterns, including captured-local
  comparisons and compile-time rejection of undefined pin targets.
- Added required-key Hash patterns to `case`, with nested Array/Hash matching,
  lowercase bindings, wildcards, pins, extra-key tolerance, and atomic commits.
- Added class-guarded object patterns to `case`, using public zero-argument
  readers with nested collection matching, pins, wildcards, and atomic commits.
- Added trailing `**remaining` Hash rest patterns to `case`, including nested
  extraction, fresh unmatched-entry Hashes, and allocation-free `**_` discard.
- Added `if` guards to `case` patterns, exposing provisional collection
  bindings inside guards while preserving mutation-free failure fallthrough.
- Added comma-separated collection-pattern alternatives with consistent
  binding-set validation, ordered short-circuiting, shared guards, and commits.
- Corrected the runtime type-ID partition so the advertised class capacity no
  longer overlaps generic-variable and interface IDs, and raised the class
  capacity from 128 to 180. Updated the self-hosted compiler's mirrored
  boundaries to match. Large package combinations such as GraphQL plus
  ActiveRecord now compile instead of misclassifying ordinary classes as
  unbound generics or exhausting class slots.

## GraphSQL 0.1.0

- Started the Diamond port of GraphSQL 0.5.1 with explicit, inheritable
  type-to-repository mappings for columns and associations, including GraphQL
  field aliases and mapped association targets.
- Added dedicated unknown-column and duplicate-aliased-association errors.
- Added lookahead-driven projection, required column/association planning,
  belongs-to and STI key retention, lazy plain eager loading, and recursive
  scoped association preloading through `GraphSQL.resolve`.
- Added `make test-graphsql-package`, including real GraphQL lookahead
  integration and coverage for STI/required-column retention.

## ActiveRecord 0.19.0

- Added `ActiveRecord::Associations::Preloader` with scalar/Array association
  names and scoped reflection loading. A target relation may filter or project
  associated rows and carry nested eager loads, enabling recursive
  GraphSQL-style query plans.

## ActiveRecord 0.18.0

- Added explicit repository association reflections, immutable lazy
  `Relation#includes`, batched `belongs_to`/`has_one`/`has_many` loading, and
  a model association cache. Unknown names raise `AssociationNotFoundError`.
- Added immutable, lazy `Relation#select`/`#reselect` projection and public
  relation query/mapper/visitor/repository metadata for GraphSQL-style query
  planning.
- Added opt-in repository column metadata, `primary_key`, `has_column?`, and
  inheritance-column metadata without changing existing constructor calls.

## Example application environments

- Added explicit development, test, and production configuration to both
  examples, with `DIAMOND_ENV` selection and `DIAMOND_DATABASE_PATH` overrides.
- Preserved the existing database filenames for development while assigning
  isolated test and production databases.
- Pinned smoke tests and benchmarks to the test environment so destructive
  fixture setup no longer wipes development data.
- Added environment-aware project-board log levels through `LOG_LEVEL`.

## Example application layout

- Reorganized the library and project-board examples into Rails-like
  `lib/models`, `lib/controllers`, and `lib/views` directories, with project
  board authentication/logging helpers under `lib/helpers`.
- Updated template compilation, application boot files, benchmarks,
  documentation, and generated-view ignore paths for the new structure.

## Log Viewer 0.1.0

- Added `diamond-log`, a stdin/file NDJSON formatter that presents logger
  metadata, request correlation, sub-millisecond timings, query details, and
  arbitrary application fields in a compact human-readable line.
- Invalid input remains visible with an `[unparsed]` marker; optional strict
  mode exits nonzero after processing a damaged or mixed stream.

## Logger 0.3.0

- Added an `off` level above `error`, allowing applications and benchmarks to
  retain instrumentation call sites while the logger performs no formatting
  or output.
- Added a configurable, multithreaded Diamond load driver exercising every
  project-board route through complete authenticated CRUD/CSRF journeys, plus
  a six-worker server harness and recorded full-stack results.
- Separated bcrypt from steady-state CRUD by logging in once per load thread,
  reusing its real session and CSRF state, and logging out after all iterations.

## Gremlin 0.2.0

- Changed request-handler failure diagnostics to structured JSON so server
  errors do not contaminate applications' NDJSON log streams.

## Logger 0.2.0

- Added newline-delimited JSON output with structured fields on every level
  method, JSON-safe escaping, and reserved logger metadata.
- Kept the existing text format as the default for backward compatibility.
- Converted the authenticated project board's application, authentication,
  controller, and ActiveRecord/Arel instrumentation logs to structured JSON.
- Made the project board output a strict NDJSON stream, including setup,
  smoke-test, and Gremlin failure events, and propagated request correlation
  fields into every query executed during that request.
- Preserved floating-point monotonic-clock milliseconds for request and query
  durations, including measurements below one millisecond.

## ActiveRecord 0.17.0

- Added `ActiveRecord::InstrumentedConnection`, a transparent database
  decorator emitting subscriber events for started, completed, and failed
  queries/writes with query IDs, SQL, bind counts, result counts, and timing.
  Bind values are deliberately excluded.
- Refactored `examples/project_board` to use the package component for its
  ActiveRecord/Arel query logs instead of an app-local connection wrapper.
- Added coverage for query/write success, failure events, timing, correlation
  IDs, connection forwarding, and secret-bind omission.

## Div 0.2.0

- Added `bin/divc_all.sh`, a clean recursive batch compiler for template
  trees. It handles spaces safely, removes stale `.cache` directories only
  below the supplied tree, rejects `/`, and delegates translation to the
  existing `divc.di` compiler.
- Replaced the duplicated flat compile loops in `examples/library` and
  `examples/project_board` with thin wrappers around the shared command.
- Added package coverage for recursive discovery, nested output, path spaces,
  and stale generated-file removal.

## Dials 0.2.0

- Added ordered route-level before filters to `Router#get`/`#post`. Each
  filter receives `(request, context, params)`, returns `nil` to continue,
  or returns a response to short-circuit later filters and the action.
- Refactored `examples/project_board` to load identity in Rack middleware
  while declaring authentication filters directly on every protected route.
- Added coverage for merged path parameters, filter ordering and short
  circuiting, and isolation between public and protected routes.

## Arel 0.34.0

- Added `Arel::MariaDBVisitor`, Arel's third dialect, verified against a
  live MariaDB 11 server (`test_mariadb_dialect.di`/`.sh`). Named
  "MariaDB" rather than "MySQL" deliberately: real MySQL 8.x has no
  `RETURNING` at all, a MariaDB-only feature (since 10.5) this visitor
  does support -- calling it "MySQL" would have overclaimed for anyone
  connecting to real MySQL.
- Unlike `Arel::PostgreSQLVisitor` (where pagination was the only real
  grammar seam), MariaDB diverges enough that `render_insert`/
  `render_update`/`render_delete` needed full per-statement overrides
  rather than the shared `render_default` fallback plus a capability
  flag:
  - no `ON CONFLICT` syntax at all -- `INSERT IGNORE` (do-nothing) and
    `... ON DUPLICATE KEY UPDATE col = VALUES(col)` (do-update, the
    `excluded.col` equivalent) instead, neither taking an explicit
    conflict target. A plain column-list target is accepted and ignored;
    a real target predicate or a named-constraint target (both asking
    for something more specific than MariaDB can express) are rejected,
    same as SQLite already rejects them. `INSERT IGNORE` is honestly a
    broader mechanism than `ON CONFLICT ... DO NOTHING` -- a deliberately
    accepted semantic gap, not a hidden one;
  - `RETURNING` only works on `INSERT`/`DELETE`, not `UPDATE` (a real
    MariaDB syntax error) -- gated by a new, visitor-private `RETURNING
    on UPDATE` capability;
  - bare `INSERT ... DEFAULT VALUES` isn't valid MariaDB syntax --
    `INSERT INTO t () VALUES ()` renders instead, under the same
    `insert default values` capability name;
  - pagination needed its own "no limit" sentinel again, like SQLite, but
    a different one: `LIMIT 18446744073709551615` (2^64-1, MariaDB/
    MySQL's own documented idiom), since MariaDB has no negative-limit
    convention;
  - `explicit NULL ordering` and `write CTEs` have no MariaDB syntax at
    all and are rejected outright; `recursive CTEs` stays supported on
    the read side.
  - Quoting (backticks), per-column `DEFAULT` in a multi-row `VALUES`
    list, and integer bitwise operators all matched SQLite/PostgreSQL
    exactly.
- Covered by a self-contained rendering suite (`tests/cases/
  arel_mariadb_dialect.di`) and a live-execution opt-in suite
  (`test_mariadb_dialect.di`/`.sh`) run against a fresh podman container.
- `package.di` bumped 0.33.0 -> 0.34.0.

## Arel 0.33.0

- Resolved the four "Deferred expression decisions" from `ROADMAP.md` by
  checking each directly against the PostgreSQL dialect:
  - `Arel.cast(expression, type_name)` now accepts an optional numeric
    parameter list (e.g. `NUMERIC(10, 2)`) -- verified portable across both
    dialects, so this needed only a widened validation regex, not a
    capability gate or a new node.
  - Added `Arel.column_default()`, a new `ColumnDefault` node that fills a
    single column of a multi-row INSERT with the table's own `DEFAULT`.
    Genuinely PostgreSQL-only (SQLite's multi-row `VALUES` grammar has no
    per-column `DEFAULT` placeholder), gated behind the new
    `per-column default values` capability.
  - Added `Arel.conflict_target_on_constraint(name)`, a new
    `ConflictConstraintTarget` node rendering `ON CONFLICT ON CONSTRAINT
    name` instead of a column list. Also genuinely PostgreSQL-only, gated
    behind the new `named-constraint conflict targets` capability. Fixed a
    bug found while adding this: `Insert#on_conflict_do_nothing`/
    `#on_conflict_do_update` only special-cased `ConflictTarget`, so passing
    a `ConflictConstraintTarget` fell through into `arel_array`, silently
    wrapping it in an `Array` instead of passing it through.
  - The fourth item, additional operators beyond the measured SQLite use
    cases, stays deferred -- no concrete need has surfaced yet.
- `Arel::SQLiteVisitor#supports_extension?` now rejects these two new
  capabilities by name; every other capability (and `Arel::PostgreSQLVisitor`)
  is unaffected.
- `package.di` bumped 0.32.0 -> 0.33.0.

## Arel 0.32.0

- Nested every class under `module Arel` (`Arel::Table`, `Arel::Query`,
  `Arel::SQLiteVisitor`, `Arel::PostgreSQLVisitor`, etc., ~30 classes in
  total) instead of the flat `Arel`-prefixed naming convention used until
  now. The 4 structural interfaces (`ArelTraversalNode` and friends) stay
  top-level and unprefixed-unchanged -- Diamond's `interface` is purely
  structural (no `include`, no `implements`), so they were never affected
  by nesting and don't need to move.
- Along the way, fixed a real compiler gap this restructuring depended on:
  `class External < Module::Class`, `value is Module::Class`, and
  `rescue e: Module::Class` previously only accepted a single bare
  identifier token, so external code could never subclass, type-check
  against, or rescue a class nested inside a module -- which would have
  broken Arel's own documented dialect-visitor extension contract
  (subclassing `Arel::Visitor`/`Arel::SQLiteVisitor` from a separate file,
  exactly what Arel's own test suite already does). Fixed at the compiler
  level (`src/compiler.c`), not worked around, since it's a real language
  gap independent of Arel.
- `package.di` bumped 0.31.0 -> 0.32.0.

## Arel 0.31.0

- Added `ArelPostgreSQLVisitor`, Arel's second dialect, verified against a
  live PostgreSQL server (`packages/arel/test_postgres_dialect.di`/`.sh`,
  opt-in and outside `tests/cases/` since it needs an already-running
  external server).
- Found nearly everything Arel models is identical syntax between SQLite
  and PostgreSQL (both were modeled on Postgres's own SQL); pagination was
  the one real grammar seam (PostgreSQL accepts a bare `OFFSET` with no
  `LIMIT`, unlike SQLite's `LIMIT -1` sentinel).
- Renamed the `"SQLite integer operators"` capability to `"integer bitwise
  operators"`, since a non-SQLite visitor now claims it too.

## Arel 0.30.0

- Matched SQLite's case-insensitive name resolution when validating relation,
  join-alias, correlation, and CTE scopes.
- Rejected duplicate declarations that differ only by case while accepting
  case-variant attribute and recursive CTE references.
- Added SQLite execution coverage for case-variant relation references.

## Arel 0.29.0

- Rejected empty SQLite identifier components with a consistent
  `ArgumentError` diagnostic.
- Added coverage across tables, columns, relation and projection aliases, CTEs,
  INSERT and UPDATE columns, INSERT…SELECT targets, and conflict targets.
- Documented identifier validity as concrete dialect policy alongside quoting.

## Arel 0.28.0

- Added execution-backed injection-shaped value coverage for predicates,
  memberships, LIKE patterns, and nested queries.
- Verified positional binding for raw-fragment parameters, INSERT and UPDATE
  values, DELETE predicates, and conflict assignments while preserving target
  tables.
- Documented that `Arel.sql` parameters remain bound while its SQL fragment is
  an explicitly caller-controlled escape hatch.

## Arel 0.27.0

- Added SQLite execution coverage for quote-containing identifiers across
  reads, aliases, CTEs, INSERT, UPDATE, DELETE, and conflict targets.
- Added an execution-backed SQL-injection-shaped table-name case proving that
  structured identifiers remain a single quoted component.
- Documented the safety boundary between structured AST identifiers and the
  explicit raw-SQL compatibility APIs.

## Arel 0.26.0

- Added exact pagination bind-order coverage through derived sources, CTEs,
  EXISTS, membership, scalar subqueries, and INSERT…SELECT sources.
- Added SQLite execution coverage for paginated derived sources, CTE bodies,
  and INSERT…SELECT operations.
- Verified that nested pagination limits affect returned and inserted rows
  without changing enclosing bind order.

## Arel 0.25.0

- Added SQLite execution coverage for offset-only SELECT and compound queries.
- Rejected negative limits and offsets at construction across both query
  families.
- Locked down immutable pagination branches while retaining exact SQL and bind
  ordering.

## Arel 0.24.0

- Added overridable literal and pagination grammar seams shared by SELECT and
  compound rendering.
- Moved pagination policy into concrete dialect visitors while retaining bind
  collection and ordering through the visitor contract.
- Corrected SQLite offset-only rendering to emit `LIMIT -1 OFFSET ?` for both
  SELECT and compound queries.

## Arel 0.23.0

- Introduced `ArelVisitor` as a reusable base for traversal, relation-scope
  validation, statement dispatch, and capability diagnostics.
- Moved visitor identity, extension support, and identifier quoting into
  concrete dialect policy; portable fixtures no longer inherit SQLite policy.
- Made nested query context exception-safe so visitor instances remain reusable
  after validation and rendering failures.

## Arel 0.22.0

- Added overridable visitor entry points for compound, INSERT, UPDATE, and
  DELETE statement rendering, with SQLite-compatible default fallbacks.
- Verified that visitors can replace or wrap complete statement rendering
  while preserving exact bind order.
- Recovered function-table capacity by replacing remaining internal Arel
  callback closures with indexed loops and reusing inherited CTE setup.

## Arel 0.21.0

- Added the overridable visitor `quote_identifier(name)` protocol and routed
  SELECT sources, CTEs, expressions, conflicts, and every write manager through
  it.
- Removed direct SQLite identifier quoting from statement managers while
  preserving existing SQLite SQL and bind ordering.
- Added custom-quoting visitor coverage across SELECT, INSERT, UPDATE, and
  DELETE rendering.

## Arel 0.20.0

- Expanded named visitor capabilities to cover RETURNING, explicit NULL
  ordering, write and recursive CTEs, and SQLite integer operators.
- Added early, visitor-specific rejection across every INSERT, UPDATE, and
  DELETE rendering path without changing SQLite SQL or bind ordering.
- Defined and tested the portable query baseline for future dialect visitors.

## Arel 0.19.0

- Added ordered declarative rewrite rules to `Arel.simplify`, with structural
  matching, postorder traversal, single-pass rule composition, and validation.
- Added optional change reporting and policy rewrites across nested SELECT and
  write trees while preserving immutability and exact bind order.
- Extended policy rewriting through third-party `arel_with_children` nodes and
  defined composition with the built-in conservative simplifications.

## Arel 0.18.0

- Extended immutable child replacement to INSERT, UPDATE, and DELETE managers,
  including source queries, expression assignments, conflicts, RETURNING, and
  write CTEs with stable bind order.
- Added third-party child replacement through `arel_with_children` and
  consolidated internal comparison/reconstruction helpers to preserve function
  capacity for extension code.
- Added semantics-preserving empty-membership simplification across read and
  write trees.

## Arel 0.17.0

- Added immutable ordered-child replacement for decorators, expressions,
  predicates, joins, CTEs, conflict targets, compounds, and SELECT queries.
- Made double-negation simplification recursive across supported trees while
  preserving source nodes and bind order.
- Added structural third-party protocols for traversal, inspection, and
  equality, with further function-capacity consolidation for extension users.

## Arel 0.16.0

- Added stable child enumeration and iterative depth-first preorder traversal
  across expressions, predicates, queries, compounds, CTEs, and write managers.
- Added an optional `visit(node)` traversal protocol and conservative,
  bind-preserving double-negation simplification.
- Switched Arel Minitest cases to `run!()` exit-status checks and removed their
  redundant golden-output files.
- Recovered function-table capacity by replacing additional internal callback
  closures with loops.

## Arel 0.15.0

- Added deterministic inspection and structural equality for INSERT, UPDATE,
  and DELETE managers, including compact, function-budget-safe state snapshots.
- Added structural handling for assignments, conflict targets, DEFAULT VALUES,
  correlations, qualified stars, conflict attributes, and subquery expressions.
- Distinguished unknown third-party inspection nodes and consolidated repeated
  comparisons to stay below Diamond's bytecode and function-table ceilings.

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
- Replaced the embedded 512-entry program function array with dynamically
  growing function storage, bounded only by the existing 16-bit bytecode index,
  and added compilation and execution coverage for 600 functions.
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
  detection, and package ("cut") installation.
- Added `require_cut`, an explicit, unambiguous require form for reaching an
  installed cut (`cuts/<name>/lib/<name>.di`) that never competes with
  ordinary `require`'s own relative-file resolution -- replacing an earlier
  implicit bare-name package fallback on plain `require`.
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
- Vendored the reginold regex engine into this repository (`reginold/`)
  instead of building it from a private sibling checkout cloned in CI with a
  read-only access token.
- Batched the self-hosted lexer/parser differential harnesses into one
  Minitest-driven process each instead of spawning and recompiling from
  scratch per case, cutting the lexer suite from ~274s to ~23s.

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
- Put self-hosting into minimal-compat maintenance mode: `make test-all` now
  runs only a two-check bootstrap smoke test (self-parse, self-run) instead
  of the full ~1400-case differential corpus, which moved to the opt-in
  `make test-self-host`. The corpus itself is unavoidably slow regardless of
  test-harness batching -- the self-hosted parser re-parses all of
  `lib/core.di` through the interpreter on every case -- and the native
  language isn't stable enough yet for continuously re-paying self-hosted
  parity's cost to be worth it. See docs/roadmap.md.

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
- Fixed LSP build breakage and a `DiamondProgram` scratch-reuse memory leak
  introduced by dynamic function storage: the LSP's chunk-function accesses
  hadn't followed the array-of-pointers change, and every handler that
  recompiles a reused `DiamondProgram` (all LSP request handlers, both fuzz
  harnesses) now frees its previous function table first instead of leaking
  it on every reuse.
- Fixed call-depth limits that could otherwise allow the native C stack to
  overflow before Diamond raised `SystemStackError`.
- Fixed loader edge cases for CRLF input, nested imports, EOF diagnostics,
  directories, unreadable files, source-map bounds, and runtime trace mapping.
- Fixed method-cache invalidation, quickening/deoptimization gaps, type
  confusion in destructuring checks, TLS partial-I/O handling, and thread/GC
  lifecycle races found by focused tests and sanitizers.
