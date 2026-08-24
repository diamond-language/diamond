# One bare top-level shim per controller action -- Dials::Router stores
# ordinary Callable values, and a class-owned self. method (every
# controller action here) isn't one; see packages/dials/README.md's "Why
# controllers still need one small shim function per action" for why
# that's a real Diamond constraint, not something worth working around.
def home_action(request, context, params) = Div.html_response(200, layout_html("Library", home_html()))

def authors_index(request, context, params) = AuthorsController.index(request, context, params)
def authors_new(request, context, params) = AuthorsController.new_form(request, context, params)
def authors_create(request, context, params) = AuthorsController.create(request, context, params)
def authors_show(request, context, params) = AuthorsController.show(request, context, params)
def authors_edit(request, context, params) = AuthorsController.edit(request, context, params)
def authors_update(request, context, params) = AuthorsController.update(request, context, params)
def authors_destroy(request, context, params) = AuthorsController.destroy(request, context, params)

def books_index(request, context, params) = BooksController.index(request, context, params)
def books_new(request, context, params) = BooksController.new_form(request, context, params)
def books_available(request, context, params) = BooksController.available(request, context, params)
def books_create(request, context, params) = BooksController.create(request, context, params)
def books_show(request, context, params) = BooksController.show(request, context, params)
def books_edit(request, context, params) = BooksController.edit(request, context, params)
def books_update(request, context, params) = BooksController.update(request, context, params)
def books_destroy(request, context, params) = BooksController.destroy(request, context, params)

# Literal routes ("/authors/new", "/books/available", ...) are
# registered before their same-segment-count ":id"-capturing siblings
# ("/authors/:id") on purpose -- Dials::Router's first-match-wins
# semantics mean ":id" would otherwise swallow "new"/"available" as if
# they were an id. Zero-capture (only references other bare top-level
# functions), the same constraint RouterHolder's own per-worker
# memoization needs from its builder -- see
# packages/dials/README.md's "Wiring into rack/gremlin".
def build_router()
  router = Dials::Router.new()

  router.get("/", home_action)

  router.get("/authors/new", authors_new)
  router.get("/authors", authors_index)
  router.post("/authors", authors_create)
  router.get("/authors/:id/edit", authors_edit)
  router.post("/authors/:id/delete", authors_destroy)
  router.get("/authors/:id", authors_show)
  router.post("/authors/:id", authors_update)

  router.get("/books/new", books_new)
  router.get("/books/available", books_available)
  router.get("/books", books_index)
  router.post("/books", books_create)
  router.get("/books/:id/edit", books_edit)
  router.post("/books/:id/delete", books_destroy)
  router.get("/books/:id", books_show)
  router.post("/books/:id", books_update)

  router
end
