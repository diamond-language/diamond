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

Explicit associations use `ActiveRecord::HasMany` and `ActiveRecord::BelongsTo`:

```diamond
books = ActiveRecord::Repository.new(Arel.table("books"), map_book)
author_books = ActiveRecord::HasMany.new(books, "author_id")
author_books.all(db, author_id)

authors = ActiveRecord::Repository.new(Arel.table("authors"), map_author)
book_author = ActiveRecord::BelongsTo.new(authors)
book_author.get(db, book.author_id())
```

`ActiveRecord::BelongsTo#get` takes the child's own foreign-key value
directly (`book.author_id()` above), not the child object itself -- no
object introspection resolves it. It returns `nil`, the same "not found"
shape `ActiveRecord::Repository#find` uses, rather than an empty `Array`,
when nothing matches.

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
