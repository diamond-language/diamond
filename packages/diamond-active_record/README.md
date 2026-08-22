# diamond-active_record

An explicit, low-magic persistence layer built on Arel.

The first slice is `ActiveRecord::Repository`, which receives all metadata it
needs instead of inspecting a schema or using dynamic dispatch:

```diamond
require "../../packages/diamond-active_record/lib/diamond-active_record"

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

There's no model base class here to instrument arbitrary setters on --
`mapper` builds whatever class the caller wants, opaque to this package --
so `ActiveRecord::DirtyAttributes` tracks changes on a plain attributes
`Hash` instead, explicitly, rather than transparently on a domain object:

```diamond
row = db.query("SELECT * FROM authors WHERE id = ?", [1])[0]
dirty = ActiveRecord::DirtyAttributes.new(row)
dirty.set("country", "England")
repository.update(db, row["id"], dirty.changes()) if dirty.changed?()
```

Wrap a loaded row (or any `Hash` of known attribute values), mutate it
through `#set(key, value)` (Diamond doesn't support overloading `[]`/`[]=`
on a user-defined class, hence `#get`/`#set` rather than bracket syntax),
then ask `#changed?`/`#attribute_changed?(key)`/`#changes` before deciding
to call `#update` -- `#changes` returns exactly the `Hash` `#update`
already expects, and setting a value back to its original leaves it out
of `#changes` too. `#to_h` returns the full current attribute state.
Comparison is `==`, so it's value equality for the ordinary
`Int`/`Float`/`String`/`Bool`/`Nil` attribute values this is meant for,
but identity equality if an attribute value is itself an `Array`/`Hash`.
There is no repository integration beyond that -- `#changes` is an
ordinary `Hash`, nothing more.

There is no schema inspection, naming convention, or object introspection,
and no implicit query scope.

## `ActiveRecord::Model` -- an optional Rails-flavored layer

Everything above is deliberately explicit: repositories, associations,
and functions you call directly, in exchange for never hiding what SQL
runs. `ActiveRecord::Model` is a thin layer over exactly that machinery
(nothing else) for anyone who wants a more Rails-familiar surface --
`Author.find(db, 1)`, `author.name`, `author.save(db)` -- built out of
`Repository` underneath, not instead of it.

Two real Diamond constraints shaped what this can and can't do, verified
directly rather than assumed:

- Diamond classes have fixed, compile-time method tables -- there is no
  `define_method`/`method_missing` in the general sense Ruby has it (a
  narrower runtime `ClassName.define_method` does exist, see
  `docs/syntax.md`, but it can only expose an *already-compiled* method
  body under a new name, not synthesize new logic from a symbol like
  `:books`). So there is still no `has_many :books` macro that conjures a
  real `books` method into existence from nothing -- every method a model
  exposes, associations included, is an ordinary `def` you write
  yourself, same as any other Diamond class.
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
ada.name=("Ada Lovelace")   # attribute writers are `name=(value)`, not `name = value` --
ada.save(db)                # Diamond has no assignment-syntax sugar for a method call
ada.books(db)
ada.destroy(db)
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

A model that associates in **both** directions (`Author has_many :books`
*and* `Book belongs_to :author`) hits a real ordering wall: Diamond
resolves a class name referenced inside a method body at compile time,
so neither class's body can name the other directly -- whichever one is
declared second doesn't exist yet as far as the first one's code is
concerned. The fix is the same shape as `.configure` itself: each
association reader reads a class-variable slot filled in later, once
both classes exist, through its own `self.wire_*` method:

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
