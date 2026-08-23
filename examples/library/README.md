# examples/library

An end-to-end smoke test wiring together everything in `packages/`:
`SQLite3` (native driver) →
[`arel`](../../packages/arel/README.md) (query builder) →
[`active_record`](../../packages/active_record/README.md) (`ActiveRecord::Model`
layer) → [`gremlin`](../../packages/gremlin/README.md) (server) →
[`rack`](../../packages/rack/README.md) (middleware). Not a package
itself -- just a small app proving the pieces actually compose.

A tiny two-table library catalog: `authors` and `books`, modeled as
`ActiveRecord::Model` classes with a real `has_many`/`belongs_to`
association between them. The app includes HTML forms for creating and
editing authors and books, plus delete actions.

`app.di` is organized as plain Diamond classes throughout -- `Author`/
`Book` (models), `Page`/`TableView`/`Form`/`Response` (view rendering and
response building), `AuthorsController`/`BooksController` (one `self.`
method per route action), and `Router` (path parsing and dispatch). The
one place it's still plain functions is the rack middleware chain itself
(`route`/`logging_middleware`/`timing_middleware`/`app`) -- a real
language constraint, not a style choice: see the comment at the top of
`app.di`.

## Run it

```sh
cd examples/library
../../build/diamond setup_db.di   # creates library.db, seeds it
../../build/diamond app.di        # starts the server on :18080
```

Then, in another terminal:

```sh
curl http://127.0.0.1:18080/
curl http://127.0.0.1:18080/authors
curl http://127.0.0.1:18080/books
curl http://127.0.0.1:18080/books/available
curl http://127.0.0.1:18080/authors/1
curl http://127.0.0.1:18080/books/1
```

Or open `http://127.0.0.1:18080/` in a browser -- it's plain HTML.
Every request is logged to stdout by the `rack` logging middleware
(`GET /books -> 200`), and `library.db` is left on disk afterward
(gitignored) -- rerun `setup_db.di` any time to reset it.

The navigation links expose `/authors/new` and `/books/new`. Existing records
have edit and delete actions on their show pages; writes go through
`ActiveRecord::Model#save`/`#destroy` (`Author.create`/`Book.create` for
new records), not raw Arel managers directly. There's no cascading
delete -- deleting an author whose books still reference it leaves those
books with a dangling `author_id`; `BooksController.show` and the books
table both handle that (`author == nil`) rather than crashing.
