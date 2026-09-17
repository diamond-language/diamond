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

One real Diamond limit used to shape everything below, and is now
resolved at the language level (still worth understanding, since it
still shapes what an association reader looks like here):

- **Resolved**: `ClassName.compile_method(name, params, body_source,
  bound_values)` (`docs/internal/design.md`'s "Runtime method synthesis" section)
  compiles a method body from a runtime source string and returns a
  `Callable` for `ClassName.define_method` to install -- the piece that
  was missing before, since `define_method` alone could only expose an
  *already-compiled* body under a new name. This is what
  `Model#has_many`/`#has_one`/`#belongs_to` build on when a model wants
  a real association-reader method synthesized in `self.configure`
  rather than hand-written, e.g.:

  ```ruby
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
    callable = Author.compile_method("books", ["db"],
      "self.has_many(target_repo, \"author_id\").all(db, self.id())",
      {"target_repo": Book.repository()})
    Author.define_method("books", callable)
  end
  ```

  Still genuinely not a `has_many :books` class-body macro in the Ruby
  sense -- there's no declarative `has_many :sym` statement that expands
  into this by itself, just ordinary imperative code inside
  `self.configure` that a model can choose to write (or not: the
  original one-line `def books(db) = self.has_many(...)` form from
  README.md still works fine and is simpler for a model with only one
  or two associations). `body_source` also can't name another class
  directly (`Book` above, say) -- that's what `bound_values` is for,
  since the synthesized method body compiles as its own separate,
  isolated program with no knowledge of the real program's other
  classes. See `docs/internal/design.md` for exactly why.

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

The package itself was also renamed from `diamond-active_record` to
`active_record` (dropping the "diamond-" prefix, matching every other
package here -- arel, gremlin, http, rack -- and split from one 862-line
file into one file per class under `lib/active_record/` (`repository.di`,
`relation.di`, `model.di`, etc.), with `lib/active_record.di` now just a
thin entry point requiring each of them in turn. Both were possible
without any ordering constraints because of two compiler features that
landed the same session: forward references (a class can construct or
call a singleton method on another class declared in a file required
later) and reopening (each per-class file independently opens `module
ActiveRecord`, merging into the same module rather than erroring) -- see
`docs/roadmap.md`'s "Compiler representation" section for both. Verified
directly that require order genuinely doesn't matter (a fully reversed
require order loads and runs identically).

## Priority order for next session

Nothing queued right now. Since the previous entry here: migrations
(`ActiveRecord::Migrator.run`/`.rollback`/`.pending`/`.applied`) and a
reusable validators library (`ActiveRecord::Validators.presence`/
`.length`/`.numericality`/`.format`/`.inclusion`/`.uniqueness`/
`.combine`) landed -- see `README.md`'s new "Migrations" and
"Validators" sections. Also settled, with code, the previously-open
"do we need connection pooling" question: no, and no pooling primitive
is buildable or needed at all -- see README.md's new "Concurrency and
connections" section (a DB connection object cannot cross a
`Thread.new` boundary, confirmed against `src/vm.c`, and every DB call
is fully blocking with no fiber-yield integration, so one connection
per `gremlin_serve` worker is both the only option and already
sufficient).

Two real Diamond constraints surfaced while building migrations
specifically, both worth remembering generally, not just here:
`ClassName.method(...)` only ever resolves against a *literal* class
name written at that exact call site, entirely at compile time -- a
bare class name is not a passable runtime value at all, so a migration
had to become a plain `Hash` (mirroring `mapper`/`validator`), not a
class with `self.up`/`self.down`. And a closure nested inside a `def
self.x` singleton method does not inherit that method's own `self` --
`self.foo()` from inside such a closure raises a runtime `TypeError` --
which is why `Migrator.run`/`.rollback` inline `Transaction.run`'s own
BEGIN/COMMIT/ROLLBACK shape per migration rather than wrapping a
callback (a nested `def` redeclared on a second loop iteration also
independently raises a runtime `TypeError`, confirmed separately).
`Validators`' own closures never needed to call back into `Validators`
itself, so neither pitfall affected them.

Before that: `Model#secure_password=`/
`#authenticate`, a `has_secure_password`-style pair of instance methods
(bcrypt cost 12, no macro -- same explicit-wiring shape as `has_many`/
`has_one`/`belongs_to`), landed on top of Diamond proper gaining real
password hashing for the first time -- a native `BCrypt` class
(`.hash`/`.verify`, backed by this system's `libxcrypt`, not a vendored
implementation) and a native `SecureRandom` class (`.bytes`/`.hex`, via
OpenSSL's already-linked `RAND_bytes`) -- see `docs/syntax.md`. Also
surfaced one real Diamond gotcha worth remembering generally, not just
here: a *typed* `attr_accessor` (e.g. `password_digest: String`)
generates a getter that enforces its return type at runtime, raising on
a nil-valued ivar rather than just returning nil -- `password_digest`
above is deliberately left untyped so `#authenticate` can read a nil
digest back on a model that never called `#secure_password=`.

Revisit `README.md`'s own "an optional Rails-flavored layer" section for
what this still doesn't do (no `has_many :sym`-style macros -- see
"Explicitly not planned" below) before picking a new direction.

## Explicitly not planned (revisit only with a real, separate design pass)

- `has_many :sym`/`belongs_to :sym`/`validates ... `-style *declarative*
  macros -- no longer blocked at the language level (`compile_method`
  resolved the general metaprogramming gap this used to cite, see
  `docs/internal/design.md`'s "Runtime method synthesis" section), but a
  deliberate choice made again this session: validations landed as
  composable plain-function building blocks instead (`README.md`'s
  "Validators" section) -- closer to how `mapper`/`before_save` already
  work here than to a new declarative surface;
- schema dump/introspection (a Rails `schema.rb`-style snapshot, or any
  `information_schema`/`PRAGMA table_info` querying) -- migrations
  create/alter schema now (`README.md`'s "Migrations" section), but this
  package still states "no schema inspection... by design" in several
  places, and a dump/introspection layer would reverse that stance;
  revisit only as its own explicit decision, not a natural extension of
  migrations;
- a dialect-aware DDL builder (`create_table("books") { |t| ... }`-style)
  -- migrations use raw SQL per migration instead, matching Arel's own
  existing SELECT/INSERT/UPDATE/DELETE-only scope cut (it never renders
  CREATE/ALTER/DROP either);
- multi-database/connection-pool management -- settled, not just
  deferred: a DB connection object cannot cross a `Thread.new` boundary
  at all (confirmed against `src/vm.c`), and every DB call is fully
  blocking with no fiber-yield integration, so pooling is both
  impossible to build across threads and pointless within one (queries
  serialize per-thread regardless of connection count) -- see
  `README.md`'s "Concurrency and connections" section.
