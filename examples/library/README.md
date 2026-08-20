# examples/library

An end-to-end smoke test wiring together everything in `packages/`:
`SQLite3` (native driver) →
[`arel`](../../packages/arel/README.md) (query
builder) → [`gremlin`](../../packages/gremlin/README.md) (server) →
[`rack`](../../packages/rack/README.md) (middleware). Not a package
itself -- just a small app proving the pieces actually compose.

A tiny two-table library catalog: `authors` and `books`, with Arel joins for
the book index and association links on both detail pages. The app includes
HTML forms for creating and editing authors and books, plus delete actions.

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
have edit and delete actions on their show pages; writes use Arel insert,
update, and delete managers with SQLite execution.
