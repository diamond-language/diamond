# diamond-active_record roadmap

This is the working plan for the next session: get `ActiveRecord::Model`
looking and feeling close to real Ruby ActiveRecord, without pretending
away the real language constraints already found and documented in
`README.md`'s own "`ActiveRecord::Model`" section.

## Where things stand

`ActiveRecord::Model` (`README.md:313` onward) already gives:

- `self.find`/`.all`/`.where`/`.create`/`.find_each`/`.find_in_batches` as
  shared, inherited class methods (via wall 2's virtual `self.foo(...)`
  dispatch) -- each model only writes its own `self.repository()`/
  `self.configure(...)` pair;
- instance `#save`/`#destroy`/`#persisted?`/`#id`, dispatching through
  each model's own `#repository`/`#to_attributes` overrides;
- one-line association readers (`#has_many`/`#has_one`/`#belongs_to`),
  optimistic locking threaded automatically through `#save`, dirty
  tracking (`DirtyAttributes`), validators and before/after-save hooks
  (`Repository`), batch iteration, and nested transactions.

Two real, already-confirmed Diamond limits shape everything below:

- **No `define_method`/`method_missing` in the general sense.** There is
  no way to synthesize a new method body from a symbol at compile or
  load time (`ClassName.define_method` can only expose an
  *already-compiled* body under a new name -- see `docs/syntax.md`). So
  `has_many :books`/`validates :name, presence: true`-style declarative
  macros that conjure real methods into existence are **not reachable**
  without a genuinely new language-level metaprogramming/macro system --
  a large, separate design question, not a diamond-active_record task.
  Don't attempt this without first scoping *that* as its own language
  feature.
- **No assignment-syntax sugar for a method call.** `ada.name=("Ada
  Lovelace")` needs the explicit call parens -- `ada.name = "Ada
  Lovelace"` doesn't parse today. This is the single most visible
  difference from real Ruby in every example in `README.md`.

## Priority order for next session

### 1. Writer-call assignment sugar (language feature, do this first)

The highest-leverage single change: teach the compiler that
`receiver.identifier = expr` (no existing local/`@ivar`/`@@cvar`
assignment shape matches, and `identifier` isn't followed by `(`) desugars
into `receiver.identifier=(expr)` -- exactly the writer-method-call
`parse_invoke` (`src/compiler.c`) already knows how to compile, just
reached from different surface syntax. This is a compiler-only change
(new lookahead predicate alongside `assignment_ahead`/`index_assignment_
ahead`, new `compile_writer_call_assignment`-shaped function reusing
`parse_invoke`'s own call-emission tail), in the same spirit as this
session's indexed-compound-assignment work -- research the exact
parse-ahead disambiguation needed (a bare `obj.attr` followed by `=` needs
to be distinguished from a compound-assignment-eligible local, and from
an ordinary `obj.attr == other` comparison) before writing a plan.

Once this lands, every `ActiveRecord::Model` example in `README.md`
gets rewritten from `ada.name=("Ada Lovelace")` to the real Ruby spelling
`ada.name = "Ada Lovelace"` -- the biggest single visual win available.

### 2. A lazy, chainable `Relation` for the class-level query interface

Today `Model.where(db, conditions)` executes immediately and returns rows.
Real ActiveRecord's `where`/`order`/`limit`/`.first`/`.find_by` chain
lazily (`User.where(active: true).order(:name).limit(10)`) and only hit
the database once actually enumerated (`.to_a`, `.each`, `.first`, ...).

Design an `ActiveRecord::Relation` wrapping the same `Arel` query object
`Repository` already builds internally, exposing `.where(...)`/`.order(...)`/
`.limit(...)`/`.first(db)`/`.to_a(db)`/`.count(db)` as chain methods that
just keep composing the underlying Arel query (`Arel.from(table).where(...)`,
etc. -- all already exposed), executing only at the terminal call. `Model`'s
own `self.where` would return one of these instead of eager rows.
This is a real design task: read `packages/arel/README.md`'s own
query-builder API first (it's already immutable/chainable at the Arel
level -- `Relation` mostly needs to be a thin per-model wrapper that maps
rows through each model's own `#to_attributes`-inverse constructor,
not a new query engine).

### 3. Smaller ergonomic polish (do alongside or after #2)

- `find_by(db, conditions)` -- `where(...).first`, one line once #2 exists;
- bang methods (`save!`, `create!`, `destroy!`) raising instead of
  returning a falsy/unsaved result -- check what `#save` currently
  returns on failure first;
- `to_json`/`as_json` on `Model` instances, built from `#to_attributes`
  and the `JSON` module already in the prelude (`packages/arel`'s own
  test suites already exercise real JSON, no new plumbing needed);
- document the "scope" convention explicitly: since `scope :active, ->
  { ... }`-style macros aren't reachable (see the wall above), an
  ordinary `def self.active(db) = self.where(db, {"active": true})` on
  each model already gets the same practical result today -- worth a
  README callout so users don't go looking for a `scope` macro that
  isn't coming.

## Explicitly not planned (revisit only with a real, separate design pass)

- `has_many :sym`/`belongs_to :sym`/`validates ... `-style declarative
  macros -- blocked on a general metaprogramming/macro system, a
  language-level feature far bigger than this package;
- schema migrations / `ActiveRecord::Schema`-style DDL management --
  never been in scope for this package (Arel doesn't render `CREATE
  TABLE` either, by design);
- multi-database/connection-pool management -- no current native
  connection-pooling primitive to build on (see `docs/roadmap.md`'s
  own "Native service depth" section).
