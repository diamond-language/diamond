# active_record roadmap

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

One real, already-confirmed Diamond limit shapes everything below:

- **No `define_method`/`method_missing` in the general sense.** There is
  no way to synthesize a new method body from a symbol at compile or
  load time (`ClassName.define_method` can only expose an
  *already-compiled* body under a new name -- see `docs/syntax.md`). So
  `has_many :books`/`validates :name, presence: true`-style declarative
  macros that conjure real methods into existence are **not reachable**
  without a genuinely new language-level metaprogramming/macro system --
  a large, separate design question, not a active_record task.
  Don't attempt this without first scoping *that* as its own language
  feature.

Writer-call assignment sugar landed: `receiver.attr = expr` now desugars
at parse time (inside `parse_invoke`, `src/compiler.c`) into the
writer-method call `receiver.attr=(expr)` it always compiled to. Every
`ActiveRecord::Model` example in `README.md` has been rewritten from
`ada.name=("Ada Lovelace")` to the real Ruby spelling `ada.name = "Ada
Lovelace"`. See `docs/syntax.md`'s "Writer-call assignment sugar" section
and `tests/cases/writer_call_assignment_sugar.di` for the language-level
details (in particular: an array-literal right-hand side, `attr = [1,
2]`, needed a lookahead to avoid being misread as the pre-existing,
never-actually-used generic writer-call syntax `attr=[T](value)`).

A lazy, chainable `ActiveRecord::Relation` also landed: `Model.all`/
`Model.where` no longer take `db` and no longer execute immediately --
they return a `Relation` wrapping the same `Arel::Query` `Repository`
already builds internally, chaining `.where(...)`/`.order(...)`/
`.limit(...)`/`.take(...)`/`.skip(...)`/`.offset(...)` (all forwarded
straight to `Arel::Query`'s own already-immutable methods, see
`packages/arel/README.md`) and executing only at a terminal `.to_a(db)`/
`.first(db)`/`.count(db)`. `Repository`'s own eager `#all(db)`/
`#where(db, conditions)` are unchanged (still used by
`HasMany`/`HasOne`/`BelongsTo`); `Repository#relation()` is the new entry
point `Model.all`/`Model.where` build on. This was a breaking change to
`Model`'s class-level `self.all`/`self.where` signatures -- every in-repo
call site was updated (`tests/cases/active_record_model.di`,
`packages/active_record/test_postgres.di`) to the new
`Model.all().to_a(db)`/`Model.where(conditions).to_a(db)` shape. See
`tests/cases/active_record_relation.di` and `README.md`'s new
`ActiveRecord::Relation` section.

The smaller ergonomic-polish items landed too: `self.find_by(db,
conditions)` (`where(conditions).first(db)` in one line);
`#save!`/`#destroy!`/`self.create!` as `!`-suffixed aliases for
`#save`/`#destroy`/`self.create` (checked directly: every write here
already always raises on failure -- `ValidationError`/`StaleObjectError`
-- so unlike real ActiveRecord there's no quiet-failure mode for a bang
form to distinguish from; both spellings behave identically, provided
purely for Rails-naming familiarity); `#to_json`/`#as_json` (`#as_json`
is the plain `Hash` `#to_attributes` already returns, a subclass's
override point for a different JSON shape; `#to_json` is
`JSON.stringify(#as_json())`); and a README callout documenting the
`def self.scope_name() = self.where({...})` convention in place of a
`scope` macro. See `tests/cases/active_record_ergonomics.di` and
the README.md sections right after `ActiveRecord::Relation`.

## Priority order for next session

Nothing queued right now -- the items this file has tracked since
`README.md`'s own `ActiveRecord::Model` section landed (writer-call
sugar, `Relation`, and the ergonomic polish above) are all done. Revisit
`README.md`'s own "an optional Rails-flavored layer" section for what
this still doesn't do (no `has_many :sym`-style macros, no schema
migrations -- see "Explicitly not planned" below) before picking a new
direction.

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
