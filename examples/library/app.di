# End-to-end demo: a gremlin server, wrapped in a rack logging/timing
# middleware chain, serving HTML pages backed by ActiveRecord::Model
# classes (Author, Book) over arel queries against the SQLite3 database
# setup_db.di seeded, rendered through packages/div view templates and
# routed through packages/dials. Run `bash compile_views.sh` once, then
# `setup_db.di`, before running this file -- see README.md's "Run it".
#
# Split one file per concern under lib/{models,controllers,views}, the same pattern
# packages/rack, packages/arel, and packages/active_record already use.
#
# Require order below is not cosmetic. Two different rules are in play:
#
# - ClassName.method(...)/instance.method(...) calls, and a bare
#   ClassName.method reference with no call (routes.di's own
#   AuthorsController.show/.index/... -- see docs/syntax.md's "Bare
#   singleton method references") -- everything the controllers/the
#   models do to reach each other, and everything routes.di does to
#   reach the controllers -- resolve via the same compile-time class
#   method-table lookup either way (docs/roadmap.md's "Forward and
#   mutual calls": "receiver-based method calls resolve dynamically"),
#   so lib/controllers/authors_controller.di,
#   lib/controllers/books_controller.di, lib/models/author.di,
#   lib/models/book.di, and lib/routes.di can all reference each other
#   regardless of which one is required first.
# - A div-generated view function called by bare name (routes.di's
#   home_action calling layout_html(...)/home_html(), and every
#   controller action calling its own view, e.g. author_show_html(...))
#   is an ordinary bare top-level function call, which is NOT forward-
#   reference-safe (same section: "Bare calls resolve only previously
#   declared top-level functions in file order") -- confirmed directly
#   against a throwaway fixture before relying on it here. So every view
#   must be required before home_action (routes.di) and before whichever
#   controller calls it, and author_books_table.html specifically before
#   author_show.html. The same bare-reference rule applies to routes.di's
#   own build_router -- middleware.di's route() references it by bare
#   name, so routes.di must be required before middleware.di too.
#
# Author.configure/Book.configure are *not* called here -- middleware.di's
# app() calls them per-worker via ensure_models_configured(context), not
# once at startup. @@repository is a class variable, and gremlin_serve's
# spawned worker threads (threads > 1) each get their own independent
# VM/heap, so a one-time top-level call here would only ever land on
# whichever worker happens to run inline -- see middleware.di's own
# comment on ensure_models_configured for how this was confirmed.
require "../../packages/active_record/lib/active_record"
require "../../packages/gremlin/lib/gremlin"
require "../../packages/rack/lib/rack"
require "../../packages/div/lib/div/runtime"
require "../../packages/dials/lib/dials"

require "./lib/config/environment"
require "./lib/views/.cache/author_books_table.html"
require "./lib/views/.cache/author_show.html"
require "./lib/views/.cache/authors_table.html"
require "./lib/views/.cache/books_table.html"
require "./lib/views/.cache/book_show.html"
require "./lib/views/.cache/author_form.html"
require "./lib/views/.cache/book_form.html"
require "./lib/views/.cache/home.html"
require "./lib/views/.cache/layout.html"

require "./lib/database"
require "./lib/models/author"
require "./lib/models/book"

require "./lib/controllers/authors_controller"
require "./lib/controllers/books_controller"
require "./lib/routes"
require "./lib/middleware"

puts("listening on http://127.0.0.1:18080 in #{AppEnvironment.name()} using #{Database.path()} (Ctrl-C to stop)")
gremlin_serve(18080, app)
