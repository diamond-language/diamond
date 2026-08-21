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

Explicit associations use `ActiveRecordHasMany`:

```diamond
books = ActiveRecordRepository.new(Arel.table("books"), map_book)
author_books = ActiveRecordHasMany.new(books, "author_id")
author_books.all(db, author_id)
```

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
