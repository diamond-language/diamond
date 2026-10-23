# build_router() below passes controller actions (AuthorsController.show
# and friends) straight to Dials::Router -- a bare `self.` method
# reference, no hand-written shim needed (see docs/syntax.md's "Bare
# singleton method references" and packages/dials/README.md's
# "Controllers"). home_action is the one real shim left: there's no
# HomeController class to reference, just a direct view-rendering call.
def home_action(request, context, params) = Div.html_response(200, layout_html("Library", home_html()))

# Literal routes ("/authors/new", "/books/available", ...) are
# registered before their same-segment-count ":id"-capturing siblings
# ("/authors/:id") on purpose -- Dials::Router's first-match-wins
# semantics mean ":id" would otherwise swallow "new"/"available" as if
# they were an id. Zero-capture (a bare singleton method reference bakes
# its target class as a compile-time constant, capturing nothing; so
# does home_action, referencing only other bare top-level functions),
# the same constraint RouterHolder's own per-worker memoization needs
# from its builder -- see packages/dials/README.md's "Wiring into
# rack/gremlin".
def build_router()
  router = Dials::Router.new()

  router.get("/", home_action)

  router.get("/authors/new", AuthorsController.new_form)
  router.get("/authors", AuthorsController.index)
  router.post("/authors", AuthorsController.create)
  router.get("/authors/:id/edit", AuthorsController.edit)
  router.post("/authors/:id/delete", AuthorsController.destroy)
  router.get("/authors/:id", AuthorsController.show)
  router.post("/authors/:id", AuthorsController.update)

  router.get("/books/new", BooksController.new_form)
  router.get("/books/available", BooksController.available)
  router.get("/books", BooksController.index)
  router.post("/books", BooksController.create)
  router.get("/books/:id/edit", BooksController.edit)
  router.post("/books/:id/delete", BooksController.destroy)
  router.get("/books/:id", BooksController.show)
  router.post("/books/:id", BooksController.update)

  router
end
