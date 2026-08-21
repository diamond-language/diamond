# diamond-active_record

An explicit, low-magic persistence layer built on Arel.

The first slice is `ActiveRecordRepository`, which receives all metadata it
needs instead of inspecting a schema or using dynamic dispatch:

```diamond
require "../../packages/diamond-active_record/lib/diamond-active_record"

repository = ActiveRecordRepository.new(
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

`ActiveRecordRepository.new` takes an optional fourth argument selecting
which Arel dialect visitor to render through -- `nil` (the default) means
Arel's own default, `ArelSQLiteVisitor`. Pass `ArelPostgreSQLVisitor.new()`
explicitly for a `PostgreSQL` connection:

```diamond
repository = ActiveRecordRepository.new(
  Arel.table("authors"), map_author, "id", ArelPostgreSQLVisitor.new()
)
```

This matters even though the two dialects render identical SQL for most of
what this repository builds: Arel's own default visitor is always
`ArelSQLiteVisitor` regardless of which database `db` actually connects
to, and the two dialects do genuinely diverge for some queries (SQLite's
offset-without-limit pagination sentinel, `LIMIT -1`, is syntax PostgreSQL
rejects outright -- see
[`packages/arel/ROADMAP.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/packages/arel/ROADMAP.md)).
Leaving the visitor unset against a `PostgreSQL` connection isn't rejected
here, since most queries this repository builds happen to render
identically either way, but it's a latent correctness gap rather than a
supported combination.

Explicit associations use `ActiveRecordHasMany` and `ActiveRecordBelongsTo`:

```diamond
books = ActiveRecordRepository.new(Arel.table("books"), map_book)
author_books = ActiveRecordHasMany.new(books, "author_id")
author_books.all(db, author_id)

authors = ActiveRecordRepository.new(Arel.table("authors"), map_author)
book_author = ActiveRecordBelongsTo.new(authors)
book_author.get(db, book.author_id())
```

`ActiveRecordBelongsTo#get` takes the child's own foreign-key value
directly (`book.author_id()` above), not the child object itself -- no
object introspection resolves it. It returns `nil`, the same "not found"
shape `ActiveRecordRepository#find` uses, rather than an empty `Array`,
when nothing matches.

Neither Arel nor the database drivers expose a transaction API of their
own (`BEGIN`/`COMMIT`/`ROLLBACK` are ordinary SQL, run through the same
`#execute(sql)` every write above already uses -- see
[`docs/io.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/io.md)).
`ActiveRecordTransaction` is that one missing piece: it commits on a
normal return and rolls back and re-raises on any exception.

```diamond
ActiveRecordTransaction.run(db) do
  repository.create(db, {"name": "Grace", "country": "USA"})
  repository.update(db, 1, {"country": "England"})
end
```

There is no schema inspection, naming convention, object introspection,
validation, dirty tracking, or implicit query scope.
