# active_record

See [ROADMAP.md](ROADMAP.md) for the forward-looking plan to bring
`ActiveRecord::Model` closer to real Ruby ActiveRecord.

An explicit, low-magic persistence layer built on Arel.

The first slice is `ActiveRecord::Repository`, which receives all metadata it
needs instead of inspecting a schema or using dynamic dispatch:

```diamond
require "../../packages/active_record/lib/active_record"

repository = ActiveRecord::Repository.new(
  Arel.table("authors"),
  map_author,
  "id"
)
author = repository.find(db, 1)
repository.update(db, 1, {"country": "England"})
```

The repository supports `all`, `find`, `where`, `create`, `update`, and
`delete`. Row-to-object mapping is supplied by the application. `where`
takes a `Hash` of column name to value, ANDed together:

```diamond
repository.where(db, {"country": "UK", "active": true})
```

`ActiveRecord::Repository.new` takes an optional fourth argument selecting
which Arel dialect visitor to render through -- `nil` (the default) means
Arel's own default, `Arel::SQLiteVisitor`. Pass `Arel::PostgreSQLVisitor.new()`
explicitly for a `PostgreSQL` connection:

```diamond
repository = ActiveRecord::Repository.new(
  Arel.table("authors"), map_author, "id", Arel::PostgreSQLVisitor.new()
)
```

This matters even though the two dialects render identical SQL for most of
what this repository builds: Arel's own default visitor is always
`Arel::SQLiteVisitor` regardless of which database `db` actually connects
to, and the two dialects do genuinely diverge for some queries (SQLite's
offset-without-limit pagination sentinel, `LIMIT -1`, is syntax PostgreSQL
rejects outright -- see
[`packages/arel/ROADMAP.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/packages/arel/ROADMAP.md)).
Leaving the visitor unset against a `PostgreSQL` connection isn't rejected
here, since most queries this repository builds happen to render
identically either way, but it's a latent correctness gap rather than a
supported combination.

Explicit associations use `ActiveRecord::HasMany`, `ActiveRecord::HasOne`,
`ActiveRecord::BelongsTo`, and `ActiveRecord::HasManyThrough`:

```diamond
books = ActiveRecord::Repository.new(Arel.table("books"), map_book)
author_books = ActiveRecord::HasMany.new(books, "author_id")
author_books.all(db, author_id)

profiles = ActiveRecord::Repository.new(Arel.table("profiles"), map_profile)
author_profile = ActiveRecord::HasOne.new(profiles, "author_id")
author_profile.get(db, author_id)

authors = ActiveRecord::Repository.new(Arel.table("authors"), map_author)
book_author = ActiveRecord::BelongsTo.new(authors)
book_author.get(db, book.author_id())
```

`ActiveRecord::HasOne#get` takes the owner's own key value directly
(`author_id` above), the same explicit-argument shape `ActiveRecord::HasMany#all`
already uses -- no object introspection or naming convention resolves it.
Unlike `HasMany#all`, which returns an `Array`, it returns a single record or
`nil` when nothing matches, the same "not found" shape
`ActiveRecord::BelongsTo#get` uses, since the relationship is one-to-one on
this side.

`ActiveRecord::BelongsTo#get` takes the child's own foreign-key value
directly (`book.author_id()` above), not the child object itself -- no
object introspection resolves it. It returns `nil`, the same "not found"
shape `ActiveRecord::Repository#find` uses, rather than an empty `Array`,
when nothing matches.

Many-to-many via an explicit join table uses `ActiveRecord::HasManyThrough`,
which takes the *target* repository directly (not the join table's own
repository, if it even has one -- a join row is rarely a meaningful domain
object) plus the join table and its two foreign-key column names:

```diamond
authorships = Arel.table("authorships")
author_books = ActiveRecord::HasManyThrough.new(books, authorships, "author_id", "book_id")
author_books.all(db, author.id())
```

`#all` joins the join table to the target repository's own table (using
its own `id_column`), filters by the owner value on the join table's
owner-key column, and maps every matching target row through the target
repository's own mapper and visitor -- so results come back as the same
domain objects `books.all(db)`/`books.where(db, ...)` would produce, not
raw `Hash`es. This is why `ActiveRecord::Repository` exposes `#table`,
`#mapper`, `#visitor`, and `#id_column` as read-only accessors: not object
introspection (that principle is about not reflecting on an opaque mapped
domain object by naming convention), just this package's own repository
exposing its own explicitly-supplied configuration for another piece of
itself to build a query with.

Every association above also has a `#preload` batch form, avoiding the
N+1 query pattern a loop calling `#all`/`#get` once per owner would
produce -- one query for every owner instead:

```diamond
author_ids = [1, 2, 3]
grouped = author_books.preload(db, author_ids)
grouped[1]   # => Array of that author's books, [] if it has none
```

`HasMany#preload`/`HasManyThrough#preload` take an `Array` of owner
values and return a `Hash` of owner value -> `Array` of mapped records,
with every owner value passed in getting a key (an empty `Array`, not a
missing one, when it has no matches). `HasOne#preload` takes the same
shape but returns a mapped record or `nil` per owner, the same
"not found" shape `#get` uses -- if more than one row matches a given
owner (a data-integrity assumption `HasOne` doesn't enforce), the last
one wins, same as `#get` only ever looking at the first result of an
unordered set. `BelongsTo#preload` takes an `Array` of the *children's*
own foreign-key values (matching `#get`'s own argument, not an owner id)
and returns a `Hash` keyed the same way.

There is no association caching or attachment to the owner objects
themselves -- there's no model base class to attach a `.books`-style
reader to (`mapper` builds whatever opaque class the caller wants), so
`#preload` just returns the `Hash`; combining it with an already-loaded
list of owners (by their own id) is the caller's own explicit step, the
same "no object introspection" stance the rest of this package takes.

`ActiveRecord::Repository.new` also takes optional `validator`, `before_save`,
and `after_save` arguments -- there is no `validates`-style class macro here
(there's no model base class to hang one on), just ordinary functions,
the same as `mapper`:

```diamond
def validate_author(attributes)
  errors = []
  errors.push("name is required") if attributes["name"] == nil || attributes["name"] == ""
  errors
end

def touch_country(db, attributes, on)
  return attributes if on == :destroy
  updated = {}
  attributes.keys().each() do |key| updated[key] = attributes[key] end
  updated["country"] = attributes["country"].upcase()
  updated
end

repository = ActiveRecord::Repository.new(
  Arel.table("authors"), map_author, "id", nil, validate_author, touch_country
)
repository.create(db, {"name": "", "country": "uk"})  # raises ActiveRecord::ValidationError
repository.create(db, {"name": "Ada", "country": "uk"})  # stores country "UK"
```

`validator` is called with the caller's own attributes `Hash` and must
return an `Array` of error message Strings (empty means valid).
`#create`/`#update` run it first, before any SQL, and raise
`ActiveRecord::ValidationError` (`.errors()` for the Array, `.message()` the
joined String) on failure.

`before_save`/`after_save` run around `#create`, `#update`, and `#delete`
alike, as `callback(db, attributes, on)` -- `on` is a `Symbol`
(`:create`/`:update`/`:destroy`) telling one shared hook which operation is
running, rather than needing six separate create/update/destroy-specific
hook slots. For `#delete`, `attributes` is a one-entry `Hash`
(`{id_column => id}`), since a delete has no attributes payload of its own,
just the row it targets. `before_save` runs after validation and returns
the attributes to actually write, letting it transform values -- that
return value is used for `#create`/`#update` but ignored for `#delete`
(there's nothing to write). `after_save` runs once the operation succeeds,
with those same attributes, and its return value is always ignored.

`ActiveRecord::Repository.new` also takes an optional `lock_column`
argument turning on optimistic locking for `#update`. There is no assumed
column name (no `lock_version` convention) and no automatic version
loading -- the caller passes the version value it already has (from the
row it loaded) as `#update`'s `expected_lock_version` argument, the same
explicit-argument shape every other option here uses:

```diamond
repository = ActiveRecord::Repository.new(
  Arel.table("accounts"), map_account, "id", nil, nil, nil, nil, "lock_version"
)
account = repository.find(db, 1)
repository.update(db, 1, {"balance": 150}, account.lock_version())
```

`#update` matches `expected_lock_version` against `lock_column` in the
same `WHERE` clause as `id_column`, and bumps `lock_column` by one in the
same statement -- one round trip, not a separate check-then-write. If
nothing matched (someone else updated, or deleted, this row first),
it raises `ActiveRecord::StaleObjectError` (`.id()` for the row it
targeted, `.message()` for a description) instead of silently updating
zero rows or a different row entirely. Passing `nil` (the default) for
`expected_lock_version` while `lock_column` is configured raises
`ArgumentError` rather than skipping the check.

Neither Arel nor the database drivers expose a transaction API of their
own (`BEGIN`/`COMMIT`/`ROLLBACK` are ordinary SQL, run through the same
`#execute(sql)` every write above already uses -- see
[`docs/io.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/io.md)).
`ActiveRecord::Transaction` is that one missing piece: it commits on a
normal return and rolls back and re-raises on any exception.

```diamond
ActiveRecord::Transaction.run(db) do
  repository.create(db, {"name": "Grace", "country": "USA"})
  repository.update(db, 1, {"country": "England"})
end
```

`ActiveRecord::Transaction.run_nested(db, savepoint_name, callback)` is
for use *inside* an already-running `#run` (or any transaction the
caller already opened itself) -- databases don't support nesting a real
`BEGIN`/`COMMIT`, so this runs `callback` inside a named `SAVEPOINT`
instead, releasing it on a normal return or rolling back to it (not the
whole outer transaction) on any exception:

```diamond
ActiveRecord::Transaction.run(db) do
  repository.create(db, {"name": "Ada", "country": "UK"})
  begin
    ActiveRecord::Transaction.run_nested(db, "before_grace") do
      repository.create(db, {"name": "Grace", "country": "USA"})
      raise RuntimeError.new("oops")
    end
  rescue error: RuntimeError
    # "Ada" is still committed once the outer block returns; "Grace" is not.
  end
end
```

Verified directly that `SAVEPOINT`/`RELEASE SAVEPOINT`/`ROLLBACK TO
SAVEPOINT` are identical syntax and semantics across all three supported
dialects (SQLite, PostgreSQL, MariaDB), so `#run_nested` needs no
visitor/dialect parameter the way `Repository` does. There is no
automatic nesting detection -- there's no ambient "am I already inside a
transaction" state exposed to Diamond code to detect that with, and
guessing from some other signal would be exactly the kind of implicit
magic this package avoids everywhere else, so the caller always says
which one it means. `savepoint_name` is restricted to ASCII letters,
digits, and underscores, and can't start with a digit -- a `SAVEPOINT`
name has no bind-parameter form in any of the three dialects (the same
reason a table or column name can't be bound either), so this is
validated before ever being interpolated into SQL text, closing off that
injection surface entirely rather than trusting the caller to.

There's no model base class here to instrument arbitrary setters on --
`mapper` builds whatever class the caller wants, opaque to this package --
so `ActiveRecord::DirtyAttributes` tracks changes on a plain attributes
`Hash` instead, explicitly, rather than transparently on a domain object:

```diamond
row = db.query("SELECT * FROM authors WHERE id = ?", [1])[0]
dirty = ActiveRecord::DirtyAttributes.new(row)
dirty["country"] = "England"
repository.update(db, row["id"], dirty.changes()) if dirty.changed?()
```

Wrap a loaded row (or any `Hash` of known attribute values), mutate it
through ordinary bracket syntax (`[]`/`[]=` operator overloading -- see
`docs/syntax.md`'s "Operator overloading" section), then ask
`#changed?`/`#attribute_changed?(key)`/`#changes` before deciding
to call `#update` -- `#changes` returns exactly the `Hash` `#update`
already expects, and setting a value back to its original leaves it out
of `#changes` too. `#to_h` returns the full current attribute state.
Comparison is `==`, so it's value equality for the ordinary
`Int`/`Float`/`String`/`Bool`/`Nil` attribute values this is meant for,
but identity equality if an attribute value is itself an `Array`/`Hash`.
There is no repository integration beyond that -- `#changes` is an
ordinary `Hash`, nothing more.

`ActiveRecord::Repository#find_each`/`#find_in_batches` iterate an entire
table without loading it all into memory at once, the way `#all` does:

```diamond
def process_author(author)
  puts(author.name())
end

repository.find_each(db, process_author, 500)
```

`#find_in_batches` is the same idea, but its callback receives one `Array`
of at most `batch_size` mapped records per call instead of one record at a
time; `#find_each` is built on top of it, unwrapping each batch into
individual calls. Both page by `id_column`
(`WHERE id_column > last_seen_id ORDER BY id_column ASC LIMIT batch_size`)
rather than SQL `OFFSET` -- the same keyset-pagination strategy Rails'
own `find_in_batches` uses: an `OFFSET`-based page re-scans and discards
every earlier page's rows on the server each time, and silently skips or
repeats rows if the table is written to while iterating (a row deleted
from an earlier page shifts every later page's `OFFSET` by one). This
avoids both, at the cost of requiring `id_column` to be meaningfully
ordered (an ordinary integer primary key always is). `batch_size`
defaults to 1000 and must be at least 1. The callback, like `mapper`
and everything else in this package, is an ordinary function -- there is
no cursor object or enumerator to hold open across calls.

`ActiveRecord::Model` exposes the same two as shared, inherited
`self.find_each`/`self.find_in_batches` class methods, alongside
`self.find`/`.all`/`.where`/`.create`. Unlike `Repository`'s own `#all`/
`#where` above, `Model`'s `self.all`/`self.where` are lazy: see
`ActiveRecord::Relation` below.

There is no schema inspection, naming convention, or object introspection,
and no implicit query scope.

## Query instrumentation

`ActiveRecord::InstrumentedConnection` transparently decorates any supported
database connection. A subscriber callback receives `started`, `completed`,
and `failed` event Hashes for every `#query`/`#execute`, including a random
query ID, operation, SQL, bind count, and elapsed milliseconds. Completion
events add `rows` or `affected`; failure events add the exception message.

```diamond
def subscriber(event)
  puts("#{event["query_id"]} #{event["phase"]} #{event["sql"]}")
end

raw = SQLite3.open("app.db")
db = ActiveRecord::InstrumentedConnection.new(raw, subscriber)
Author.all().to_a(db)
```

Bind values are deliberately absent from events, making the default shape
safe for passwords, session tokens, and other secrets. The wrapper forwards
`#last_insert_row_id` and `#close`, so repositories, models, and direct Arel
execution use it without changes. It has no logger dependency: applications
choose whether the subscriber writes logs, metrics, traces, or test events.

## Validators

`Repository`'s own `validator` argument (above) is any ordinary
`attributes -> Array[String]` function -- `ActiveRecord::Validators` is a
small library of reusable ones, so a model doesn't have to hand-roll
`if attributes["name"] == nil || attributes["name"] == "" then ...` every
time. Each `self.xxx` builds and returns one such function; `#combine`
concatenates several into one for `Repository.new`'s own `validator` slot:

```diamond
validator = ActiveRecord::Validators.combine([
  ActiveRecord::Validators.presence("name"),
  ActiveRecord::Validators.length("name", maximum: 100),
  ActiveRecord::Validators.format("email", Regexp.new("^[^@]+@[^@]+$")),
])
repository = ActiveRecord::Repository.new(
  Arel.table("authors"), build_author, "id", nil, validator)
repository.create(db, {"name": "", "email": "bad"})  # raises ValidationError, 2 errors
```

There is no `validates :field, presence: true`-style class macro here,
same as everywhere else in this package -- a model wires these up
explicitly, the same as `mapper`/`before_save`/`after_save` already are.

- `presence(field, message = nil)` -- fails on `nil` or `""`.
- `length(field, minimum = nil, maximum = nil, message = nil)` -- checks
  `value.length()` (works for `String`/`Array`/`Hash` alike); `nil`
  passes silently (that's `presence`'s job, not this one's).
- `numericality(field, message = nil)` -- Diamond has no `is_a?`/`class()`
  runtime type check (confirmed: neither exists), so this is duck-typed
  instead: `value + 0` inside a `rescue error: TypeError`, the same
  reasoning `nil`/a `String` both fail this and an `Int`/`Float` both
  pass it. `nil` fails (matching real ActiveRecord's own
  `allow_nil: false` default) -- pair with `presence` if `nil` should be
  reported as "required" instead of "must be numeric".
- `format(field, pattern, message = nil)` -- `pattern` is an ordinary
  `Regexp`, matched with `#match?`. `nil` fails.
- `inclusion(field, values, message = nil)` -- `values.include?(value)`.
- `uniqueness(db, table, field, visitor = nil, message = nil)` -- the one
  check needing a real query. `Repository`'s own `validator` is called as
  `@validator(attributes)`, never `(db, attributes)`, so this closes over
  `db` directly instead (an ordinary captured value, built where `db` is
  already in scope -- typically `self.configure`), rather than changing
  `Repository`'s signature. **Only correct for `#create`**: `Repository`'s
  validator has no access to the row's own `id` (or even whether this is
  a `#create` or `#update` at all), so on `#update` this will also flag a
  record whose unique field is unchanged as conflicting with itself -- a
  real `Repository`-level constraint, not something `uniqueness` works
  around.
- `combine(validators)` -- runs every validator in `validators` and
  concatenates their error `Array`s, standing in for what several
  `validates` calls would do declaratively in real ActiveRecord.

Each `self.xxx` returns a nested `def` closing over its own arguments
(field name, options, message) -- the same closure-returning shape
`packages/rack`'s `rack_terminal_wrap` already uses. One real limit worth
knowing if writing a new validator of this shape: a closure nested inside
a `def self.x` singleton method does **not** inherit that method's own
`self` -- a `self.foo()` call from inside such a closure raises a runtime
`TypeError` (confirmed directly). None of the validators above need to
call back into `Validators` itself, so this doesn't affect them, but it's
the reason `ActiveRecord::Migrator` (below) never routes through
`ActiveRecord::Transaction.run`.

## Migrations

Versioned, ordered schema changes -- the piece connecting `Repository`/
`Transaction` (which both assume a schema already exists) to a database
that actually evolves over time. Deliberately raw SQL, not a
dialect-aware DDL builder: Arel itself only ever renders
SELECT/INSERT/UPDATE/DELETE, never CREATE/ALTER/DROP, and this follows
that same scope cut rather than inventing a second, DDL-flavored query
builder on top.

A migration is a plain `Hash`, not a class -- the same shape `mapper`/
`validator`/`before_save`/`after_save` already are (an ordinary function
reference, stored and invoked as a value). This isn't a style choice: a
bare class name is not a passable runtime value in Diamond at all --
`ClassName.method(...)` only ever resolves against a *literal* class name
written at that exact call site, entirely at compile time (confirmed
directly), and `ActiveRecord::Migrator` only ever sees a migration as one
element of a generic runtime `Array`, so a class-shaped migration could
never have its methods invoked dynamically the way a real migration
needs.

```diamond
def create_authors_up(db)
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT, country TEXT)")
end
def create_authors_down(db)
  db.execute("DROP TABLE authors")
end
def create_authors_migration() = {
  "version": "20260101120000", "up": create_authors_up, "down": create_authors_down
}
```

`"down"` is optional -- a missing key reads back `nil` (an ordinary
lookup, not an error), and `#rollback` raises a clear error rather than
silently no-opping if it's ever actually called for an irreversible
migration. `"version"` only has to be a stable, unique `String` -- nothing
here sorts or compares version values at all (Diamond's `String` has no
ordering comparison: `<`/`>` raise `TypeError`, `<=>` returns `nil`,
confirmed directly), so a caller's own `Array` order is the only order
that matters, timestamp-shaped or not.

```diamond
# create_books_migration defined the same way, its own "up"/"down" pair
migrations = [create_authors_migration(), create_books_migration()]
ActiveRecord::Migrator.run(db, migrations)
```

`#run(db, migrations, visitor = nil)` ensures a `schema_migrations`
bookkeeping table exists (`CREATE TABLE IF NOT EXISTS`, identical syntax
across all four supported dialects), rejects a duplicate `"version"`
anywhere in `migrations`, then walks `migrations` in the given `Array`'s
own order applying every one not yet recorded -- each inside its own
transaction, so a failure partway through a multi-migration run leaves
every prior migration committed and correctly excluded from the next
`#run`. Safe to call repeatedly: already-applied migrations are skipped,
never re-run.

`#rollback(db, migrations, steps = 1, visitor = nil)` walks `migrations`
in *reverse* `Array` order, collecting the first `steps` whose version is
currently applied, and reverses each (also inside its own transaction) --
raising if any of them has no `"down"` entry rather than silently
skipping it.

`#pending(db, migrations)` / `#applied(db, migrations)` return the
not-yet-applied / already-applied subset of `migrations`, in the given
`Array`'s own order -- cheap status helpers for a project's own migration
script to print before running anything.

**No directory scanning.** Diamond has no directory-listing/glob
primitive at all, and this package wouldn't use one even if it existed --
a consuming project explicitly `require`s each migration file (ordinary
compile-time `require`, one line per file) and lists them in order
itself:

```diamond
# db/migrate.di, in a project using active_record
require "./migrate/20260101120000_create_authors"
require "./migrate/20260102093000_create_books"

ActiveRecord::Migrator.run(db, [create_authors_migration(), create_books_migration()])
```

## Concurrency and connections

`packages/gremlin`'s `threads: N` already gives each worker its own
independent heap -- and, checked directly against `src/vm.c`, a DB
connection object cannot cross a `Thread.new` boundary at all
(`copy_value_into_vm` rejects `DIAMOND_OBJECT_SQLITE3`/`POSTGRES`/`MYSQL`,
same bucket as `File`/`Socket`/`Listener`). There is therefore no way to
share one open connection across threads, and no connection-pooling
primitive to build here even if that were desired. This isn't a gap:
every DB call in this package (`sqlite3_step`/`PQexecParams`/MySQL's
query calls) is fully synchronous C with no fiber-yield integration the
way sockets have, so a query blocks the *entire* worker thread for its
duration regardless of how many connection objects that thread holds --
meaning one connection opened once per `gremlin_serve` worker, reused for
every request that worker ever handles, is both the only option and
already sufficient. Scaling DB throughput is `threads: N`, not a pool.
Worth knowing as a real limitation: a slow query on one worker stalls
every other connection that worker is concurrently juggling, not just
the one that issued it.

## `ActiveRecord::Model` -- an optional Rails-flavored layer

Everything above is deliberately explicit: repositories, associations,
and functions you call directly, in exchange for never hiding what SQL
runs. `ActiveRecord::Model` is a thin layer over exactly that machinery
(nothing else) for anyone who wants a more Rails-familiar surface --
`Author.find(db, 1)`, `author.name`, `author.save(db)` -- built out of
`Repository` underneath, not instead of it.

Two real Diamond constraints shaped what this can and can't do, verified
directly rather than assumed:

- Diamond classes have fixed, compile-time method tables, and there is
  still no `has_many :books`-style *class-body macro* that would expand
  into a real method by itself -- but `ClassName.compile_method` (see
  `docs/design.md`'s "Runtime method synthesis" section) can now compile
  a method body from a runtime string and, together with
  `ClassName.define_method`, attach it to an already-loaded class. A
  model that wants a synthesized association reader writes ordinary
  imperative code in its own `self.configure` to build one -- see
  "Association readers" below for a worked `has_many` example -- every
  method a model exposes is still either a hand-written `def`, or
  explicitly synthesized this way, nothing conjures one implicitly from
  a bare symbol.
- Instance methods dispatch virtually (confirmed directly: a shared
  method calling `self.foo()` correctly reaches a subclass's override).
  `def self.x` class methods originally couldn't do this at all -- `self`
  wasn't even accessible inside one -- but this has since been fixed at
  the language level (see `docs/syntax.md`/`docs/design.md`): `self`
  inside a class-owned singleton method now holds the actual receiver
  class, and `self.foo(...)` there dispatches virtually the same way an
  instance method's `self.foo()` already did. This is what lets `Model`
  itself provide `self.find`/`.all`/`.where`/`.create` as shared,
  inherited methods below, rather than requiring each model to write its
  own copies.

```diamond
class Author < ActiveRecord::Model
  attr_accessor name: String, country: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @name = attributes["name"]
    @country = attributes["country"]
  end

  def to_attributes() = {"name": @name, "country": @country}
  def repository() = @@repository

  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def books(db) = self.has_many(Book.repository(), "author_id").all(db, self.id())
end

def build_author(row) = Author.new(row)
Author.configure(ActiveRecord::Repository.new(Arel.table("authors"), build_author, "id"))
```

```diamond
Author.create(db, {"name": "Ada", "country": "UK"})  # Model's own self.create, inherited
ada = Author.find(db, 1)                             # likewise self.find
ada.name = "Ada Lovelace"
ada.save(db)
ada.books(db)
ada.destroy(db)
```

`self.all`/`self.where` are lazy and chainable -- unlike every other
`Model` class method above, neither takes `db`, since nothing runs until
a terminal call on the `ActiveRecord::Relation` they return:

```diamond
name = Arel.table("authors").column("name")
uk_authors = Author.where({"country": "UK"}).order(name.asc()).limit(10)
uk_authors.to_a(db)                         # only now does any SQL actually run

Author.where({"country": "UK"}).first(db)   # a single Author, or nil
Author.all().count(db)                      # an Int
```

`ActiveRecord::Relation` is a thin wrapper over the same `Arel::Query`
`Repository` already builds internally (see `packages/arel/README.md` --
`Arel::Query` is itself already immutable and chainable, so `Relation`
adds no query-building logic of its own, just `#where`/`#order`/`#take`/
`#limit`/`#skip`/`#offset` forwarding to it and mapping rows through this
model's own row-mapper function at the three terminal calls, `#to_a(db)`/
`#first(db)`/`#count(db)`). Deliberately, `#first(db)` does **not** add an
implicit `ORDER BY` the way real ActiveRecord's does -- there is no schema
inspection or naming convention here to build one from, so `#first`
without a preceding `.order(...)` returns whatever row the database
happens to return first.

`self.find_by(db, conditions)` is `where(conditions).first(db)` in one
line -- a single matching instance, or `nil`, with the same "no implicit
`ORDER BY`" caveat as `#first` itself.

`#save!`/`#destroy!`/`self.create!` are `!`-suffixed aliases for
`#save`/`#destroy`/`self.create`, provided purely for Rails-naming
familiarity. Unlike real ActiveRecord, where the plain (non-`!`) forms
swallow a validation failure and return `false` while the `!` forms
raise, every write here (`#save`/`#destroy`/`self.create`, and by
extension `Repository#create`/`#update`/`#delete` underneath them)
already always raises on failure (`ActiveRecord::ValidationError`,
`ActiveRecord::StaleObjectError`) -- there is no quiet failure mode to
distinguish a bang form from. Both spellings behave identically; use
whichever reads better at the call site.

`#to_json`/`#as_json` build on `#to_attributes` and the `JSON` module
already in the prelude -- `#as_json` is the same plain `Hash`
`#to_attributes` already returns (a subclass can override just this one
method for a different JSON shape -- dropping a column, embedding an
association -- without touching `#to_json` itself), and `#to_json` is
`JSON.stringify(#as_json())`.

Since `has_many :sym`/`validates ...`-style declarative macros aren't
reachable (see the two real Diamond constraints above), there is no
`scope` macro either -- an ordinary class method already gets the same
practical result:

```diamond
class Author < ActiveRecord::Model
  ...
  def self.uk() = self.where({"country": "UK"})
end

Author.uk().order(...).to_a(db)
```

Every subclass overrides two **instance** methods (`#repository`,
`#to_attributes`) so `Model`'s shared `#save`/`#destroy`/`#persisted?`/
`#id` reach them through real virtual dispatch. `self.find`/`.all`/
`.where`/`.create` are shared the same way, at the **class** level, and
don't need to be repeated per model. The one thing every model still
writes for itself is `self.repository()`/`self.configure(repository)` --
`@@repository` is a class variable, and Diamond scopes `@@cvar` storage
to whichever class the code reading/writing it is defined in, not the
receiver a call was made through, so an inherited `self.repository()`
reading `Model`'s own `@@repository` would give every subclass the same
shared slot instead of its own; each model needs its own copy of just
these two lines to get its own isolated slot. `#to_attributes` is the
reverse of a `Repository`'s own
`mapper` function -- this instance's current field values as the same
plain `Hash` `#create`/`#update` already write. `#save` picks `#create`
or `#update` based on `#persisted?` (does `@attributes` have an
`id_column` key yet), and after a successful `#create` sets that key
from `db.last_insert_row_id()` so a later `#save`/`#destroy` on the same
instance does the right thing. `#save` also threads optimistic locking
through automatically when the repository has a `lock_column`
configured, using whatever value is already in `@attributes` as
`expected_lock_version` -- see the optimistic locking section above.

Association readers are one line each, built from `Model#has_many`/
`#has_one`/`#belongs_to` (thin wrappers constructing the same
`HasMany`/`HasOne`/`BelongsTo` objects described earlier, nothing new):

```diamond
def books(db) = self.has_many(Book.repository(), "author_id").all(db, self.id())
def profile(db) = self.has_one(Profile.repository(), "author_id").get(db, self.id())
```

The one-liner above is still the simplest way to write an association
reader, and is what most models should reach for. `ClassName.compile_method`
(`docs/design.md`'s "Runtime method synthesis" section) offers a second,
*synthesized* way to build the same shape of method from `self.configure`
instead of a hand-written `def` -- useful mainly when a model wants to
build several similar readers programmatically rather than writing one
`def` per association:

```diamond
class Author < ActiveRecord::Model
  ...
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
    # body_source can't name Book directly -- it compiles as its own
    # isolated program with no knowledge of the real one's other
    # classes (see docs/design.md) -- so Book.repository() is evaluated
    # here, in this method's own real compile, and threaded in via
    # bound_values instead.
    callable = Author.compile_method("books", ["db"],
      "self.has_many(target_repo, \"author_id\").all(db, self.id())",
      {"target_repo": Book.repository()})
    Author.define_method("books", callable)
  end
end
```

`ada.books(db)` then works exactly like the hand-written version above --
same method, same dispatch, the only difference is where its body came
from. Still not a `has_many :books` class-body macro (there's no
declarative statement that expands into this for you); it's ordinary
Diamond code that happens to build a method instead of calling one.

A model that associates in **both** directions (`Author has_many :books`
*and* `Book belongs_to :author`) used to hit a real ordering wall: Diamond
used to resolve a class name referenced inside a method body only against
whatever had already been compiled earlier in the file, so neither
class's body could name the other directly -- whichever one was declared
second didn't exist yet as far as the first one's code was concerned.
This has since been fixed at the language level (`docs/roadmap.md`'s
"Compiler representation" section): `Author#books` can now call
`Book.repository()` directly, and `Book#author` can call
`Author.repository()` directly, regardless of which class is declared
first, or that each needs the other. The `wire_*` pattern below still
works exactly as shown (nothing about it broke), but a new model no
longer needs it just to solve this specific ordering problem -- it
remains useful only if a model wants an association's target repository
resolved through some *other* indirection than a plain class-variable
read/write, the same shape as `.configure` itself:

```diamond
class Author < ActiveRecord::Model
  ...
  def books(db) = self.has_many(@@books_repository, "author_id").all(db, self.id())
  def self.wire_books(repository: ActiveRecord::Repository)
    @@books_repository = repository
  end
end

class Book < ActiveRecord::Model
  ...
  def author(db) = self.belongs_to(@@author_repository).get(db, @author_id)
  def self.wire_author(repository: ActiveRecord::Repository)
    @@author_repository = repository
  end
end

Author.wire_books(Book.repository())
Book.wire_author(Author.repository())
```

There is still no schema inspection, naming convention, or object
introspection here -- `attr_accessor`, `#to_attributes`, and every
forwarder above are things you write, not things this layer infers.

### `has_secure_password`-style password hashing

`Model#secure_password=`/`#authenticate` are a `has_secure_password`-style
pair of instance methods, built on the native `BCrypt` class (bcrypt
password hashing, backed by this system's `libxcrypt`, not a vendored
implementation -- see `docs/syntax.md`). Like every association reader
above, this is **not** a macro -- there is no `has_secure_password
:password` conjuring a `password_digest` column or new methods into
existence from a symbol, for the same reason `has_many`/`has_one`/
`belongs_to` aren't macros either. A model wires it up the same explicit
way it wires up every other column:

```diamond
class User < ActiveRecord::Model
  # password_digest deliberately untyped, not `: String` -- a user that
  # never called #secure_password= (an invited-but-not-yet-onboarded
  # account, say) has a nil digest, and a *typed* attr_accessor's
  # generated getter enforces its return type at runtime, raising on a
  # nil read rather than just returning nil the way #authenticate needs.
  attr_accessor email: String, password_digest

  def initialize(attributes: Hash = {})
    super(attributes)
    @email = attributes["email"]
    @password_digest = attributes["password_digest"]
  end

  def to_attributes() = {"email": @email, "password_digest": @password_digest}
  def repository() = @@repository

  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
end

User.configure(ActiveRecord::Repository.new(Arel.table("users"), build_user, "id"))

user = User.new({"email": "ada@example.com"})
user.secure_password = "hunter2"   # writer-call sugar -> secure_password=(...)
user.save(db)

user.authenticate("hunter2")       # => true
user.authenticate("wrong")         # => false
```

`#secure_password=` hashes with `BCrypt.hash(password, 12)` (bcrypt cost
12, matching Rails' own `BCrypt::Engine::DEFAULT_COST`) and writes the
result through `self.password_digest=(...)` -- the ordinary writer
`attr_accessor` already generated, not a direct `@password_digest`
write, the same virtual `self.foo(...)` dispatch `#save`/`#destroy`/`#id`
already rely on to reach whatever a subclass's own accessor generated.
The cost isn't configurable through this helper; call `BCrypt.hash(password,
cost)` directly for a different one. `#authenticate` reads back through
`self.password_digest()` and returns `false` (not an exception) both when
the stored digest is `nil` (nothing set yet) and when it's a well-formed
digest that just doesn't match -- there is no quiet-vs-raising distinction
to make here, unlike `#save`/`#destroy` elsewhere in this package, since a
failed authentication attempt is an entirely ordinary outcome, not a
programmer error.
