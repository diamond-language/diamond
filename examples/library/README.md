# examples/library

An end-to-end smoke test wiring together everything in `packages/`:
`SQLite3` (native driver) →
[`arel`](../../packages/arel/README.md) (query builder) →
[`active_record`](../../packages/active_record/README.md) (`ActiveRecord::Model`
layer) → [`gremlin`](../../packages/gremlin/README.md) (server) →
[`rack`](../../packages/rack/README.md) (middleware) →
[`dials`](../../packages/dials/README.md) (routing/controllers) →
[`div`](../../packages/div/README.md) (view templates). Not a package
itself -- just a small app proving the pieces actually compose.

A tiny two-table library catalog: `authors` and `books`, modeled as
`ActiveRecord::Model` classes with a real `has_many`/`belongs_to`
association between them. The app includes HTML forms for creating and
editing authors and books, plus delete actions.

## Layout

Split one file per concern, the same pattern `packages/rack`,
`packages/arel`, and `packages/active_record` already use:

```
app.di                          entry point: ordered requires, then starts the server
compile_views.sh                batch-compiles lib/views/*.html.div (run first)
setup_db.di                     creates and seeds library.db
lib/
  database.di                   per-worker SQLite connection
  models/
    author.di, book.di          ActiveRecord::Model classes
  controllers/
    authors_controller.di       AuthorsController -- one self. method per route action,
                                 (request, context, params)
    books_controller.di         BooksController, same
  views/
    layout.html.div             page shell + nav, wraps every response
    home.html.div, authors_table.html.div, books_table.html.div,
    author_books_table.html.div, author_show.html.div, book_show.html.div,
    author_form.html.div, book_form.html.div
  routes.di                     one bare top-level shim function per controller action, plus
                                 build_router() -- see packages/dials/README.md's "Why
                                 controllers still need one small shim function per action"
  middleware.di                 route/logging_middleware/timing_middleware/app -- still plain
                                 top-level functions, a real language constraint (see the
                                 comment at the top of that file), not a style choice
```

`app.di`'s own `require` block is ordered deliberately, not alphabetically
-- see the comment at its top for exactly which parts of that order are
load-bearing (views before the controllers that call them, and
`author_books_table.html` before `author_show.html`, which calls it as a
partial).

## Routing

Requests are dispatched through a [`packages/dials`](../../packages/dials/README.md)
`Dials::Router` (`lib/routes.di`'s `build_router()`), replacing what used
to be a hand-written if/elsif chain (`Router.dispatch`). Path segments
like `/authors/:id` capture into a `params` Hash merged with query/form
params, so controller actions now take `(request, context, params)`
instead of manually parsing an `id` out of the path themselves.
Literal routes (`/authors/new`, `/books/available`) are registered
*before* their same-shaped `:id`-capturing siblings on purpose -- first-
match-wins semantics mean `:id` would otherwise swallow `"new"` as if it
were an id. `Dials::Response` replaced this app's own former `Response`
class verbatim (same three methods, just relocated to a real package);
`Dials::Params.parse` similarly replaced `Form.parse`.

## Views

Every page renders through a [`packages/div`](../../packages/div/README.md)
template: `lib/views/*.html.div` source compiles to an ordinary `.di` function
(`author_show.html.div` -> `author_show_html(...)`), which a controller
calls directly and wraps with `layout_html(title, content)`, then hands
to `Div.html_response(200, ...)` for the actual `[status, headers, body]`
Rack response -- the documented `div`+`rack` integration point.

This replaced the app's previous hand-built-string views (`Page`/
`TableView`/generic `Form.render`), which is a real, not just cosmetic,
improvement: every value interpolated into HTML there was **unescaped**
raw string concatenation. `<%= %>` escapes by default now -- try editing
an author's name to include `<b>` through the edit form; it renders as
literal text, not bold. The old generic `Form.render(fields: Array, ...)`
abstraction is gone too, replaced by two concrete templates
(`author_form.html.div`, `book_form.html.div`) -- it only ever existed to
compensate for not having a real template engine, and `BooksController`'s
own form already couldn't use it (its `<select>` didn't fit the generic
shape), so nothing is lost by making both forms concrete.

## Run it

```sh
cd examples/library
bash compile_views.sh              # compiles lib/views/*.html.div (once, or after editing a view)
../../build/diamond setup_db.di    # creates library.db, seeds it
../../build/diamond app.di         # starts the server on :18080
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
(gitignored) -- rerun `setup_db.di` any time to reset it. The generated
`lib/views/.cache/*.html.di` files are gitignored too -- `compile_views.sh` re-runs
in well under a second, so nothing is lost by not committing them.

The navigation links expose `/authors/new` and `/books/new`. Existing records
have edit and delete actions on their show pages; writes go through
`ActiveRecord::Model#save`/`#destroy` (`Author.create`/`Book.create` for
new records), not raw Arel managers directly. There's no cascading
delete -- deleting an author whose books still reference it leaves those
books with a dangling `author_id`; `BooksController.show` and the books
table both handle that (`author == nil`) rather than crashing.
