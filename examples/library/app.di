# End-to-end demo: a gremlin server, wrapped in a rack logging/timing
# middleware chain, serving HTML pages backed by ActiveRecord::Model
# classes (Author, Book) over arel queries against the SQLite3 database
# setup_db.di seeded, rendered through packages/div view templates. Run
# `bash compile_views.sh` once, then `setup_db.di`, before running this
# file -- see README.md's "Run it".
#
# Split one file per concern under lib/ and views/, the same pattern
# packages/rack, packages/arel, and packages/active_record already use.
#
# Require order below is not cosmetic. Two different rules are in play:
#
# - ClassName.method(...)/instance.method(...) calls (everything
#   Router/the controllers/the models do to reach each other) resolve
#   dynamically, not at compile time (docs/roadmap.md's "Forward and
#   mutual calls": "receiver-based method calls resolve dynamically") --
#   so lib/router.di, lib/authors_controller.di, lib/books_controller.di,
#   lib/author.di, and lib/book.di can reference each other regardless of
#   which one is required first.
# - A div-generated view function calling *another* div-generated
#   function by bare name (views/author_show.html.div calling
#   author_books_table_html(...) as a partial) is an ordinary bare top-
#   level function call, which is NOT forward-reference-safe (same
#   section: "Bare calls resolve only previously declared top-level
#   functions in file order") -- confirmed directly against a throwaway
#   fixture before relying on it here. That's also true of every
#   controller's own bare calls into the view layer
#   (author_show_html(...), layout_html(...), etc.). So every view must
#   be required before whichever controller calls it, and
#   author_books_table.html specifically before author_show.html.
require "../../packages/active_record/lib/active_record"
require "../../packages/gremlin/lib/gremlin"
require "../../packages/rack/lib/rack"
require "../../packages/div/lib/div/runtime"

require "./views/.cache/author_books_table.html"
require "./views/.cache/author_show.html"
require "./views/.cache/authors_table.html"
require "./views/.cache/books_table.html"
require "./views/.cache/book_show.html"
require "./views/.cache/author_form.html"
require "./views/.cache/book_form.html"
require "./views/.cache/home.html"
require "./views/.cache/layout.html"

require "./lib/database"
require "./lib/author"
require "./lib/book"
require "./lib/response"
require "./lib/form"

require "./lib/authors_controller"
require "./lib/books_controller"
require "./lib/router"
require "./lib/middleware"

Author.configure(ActiveRecord::Repository.new(Arel.table("authors"), build_author, "id"))
Book.configure(ActiveRecord::Repository.new(Arel.table("books"), build_book, "id"))

puts("listening on http://127.0.0.1:18080 (Ctrl-C to stop)")
gremlin_serve(18080, app)
