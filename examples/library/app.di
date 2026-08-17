# End-to-end demo: a gremlin server, wrapped in a rack logging
# middleware, serving HTML pages backed by arel queries against the
# SQLite3 database setup_db.di seeded. Run setup_db.di first.
#
# gremlin_serve gives each worker its own persistent `context` Hash
# (see packages/gremlin/gremlin.di's "Per-worker context") -- the db
# connection is opened lazily on this worker's first request and
# stashed there, reused by every request after.

require "../../packages/arel/arel"
require "../../packages/gremlin/gremlin"
require "../../packages/rack/rack"

def db_path()
  "library.db"
end

def get_db(context)
  db = context["db"]
  if db == nil
    db = SQLite3.open(db_path())
    context["db"] = db
  end
  db
end

def nav()
  "<p><a href=\"/\">Home</a> | <a href=\"/authors\">Authors</a> | " +
  "<a href=\"/books\">Books</a> | " +
  "<a href=\"/books/available\">Available books</a></p>"
end

def render_page(title, body)
  "<!DOCTYPE html><html><head><title>#{title}</title></head>" +
  "<body><h1>#{title}</h1>#{nav()}#{body}</body></html>"
end

def render_authors_table(authors)
  rows = ""
  def render_row(author)
    rows = rows + "<tr><td>#{author["id"]}</td><td>#{author["name"]}</td><td>#{author["country"]}</td></tr>"
  end
  authors.each(render_row)
  "<table border=\"1\"><tr><th>ID</th><th>Name</th><th>Country</th></tr>#{rows}</table>"
end

def load_authors_by_id(db)
  authors = Arel.from("authors").to_a(db)
  by_id = {}
  def index_author(author)
    by_id[author["id"]] = author["name"]
  end
  authors.each(index_author)
  by_id
end

def render_books_table(books, authors_by_id)
  rows = ""
  def render_row(book)
    author_name = authors_by_id[book["author_id"]]
    status = if book["available"] == 1 then "yes" else "no" end
    rows = rows + "<tr><td>#{book["title"]}</td><td>#{author_name}</td>" +
                  "<td>#{book["year"]}</td><td>#{status}</td></tr>"
  end
  books.each(render_row)
  "<table border=\"1\"><tr><th>Title</th><th>Author</th><th>Year</th>" +
  "<th>Available</th></tr>#{rows}</table>"
end

def home_handler(request, context)
  body = "<p>A tiny end-to-end demo: SQLite3 + arel + gremlin + rack.</p>"
  [200, {"Content-Type": "text/html"}, render_page("Library", body)]
end

def authors_handler(request, context)
  db = get_db(context)
  authors = Arel.from("authors").order("name").to_a(db)
  [200, {"Content-Type": "text/html"}, render_page("Authors", render_authors_table(authors))]
end

def books_handler(request, context)
  db = get_db(context)
  books = Arel.from("books").order("year").to_a(db)
  body = render_books_table(books, load_authors_by_id(db))
  [200, {"Content-Type": "text/html"}, render_page("Books", body)]
end

def available_books_handler(request, context)
  db = get_db(context)
  books = Arel.from("books").where({"available": 1}).order("year").to_a(db)
  body = render_books_table(books, load_authors_by_id(db))
  [200, {"Content-Type": "text/html"}, render_page("Available books", body)]
end

def not_found_handler(request, context)
  [404, {"Content-Type": "text/plain"}, "not found: #{request["path"]}"]
end

def route(request, context)
  path = request["path"]
  if path == "/"
    home_handler(request, context)
  elsif path == "/authors"
    authors_handler(request, context)
  elsif path == "/books"
    books_handler(request, context)
  elsif path == "/books/available"
    available_books_handler(request, context)
  else
    not_found_handler(request, context)
  end
end

def logging_middleware(request, context, forward)
  response = forward(request, context)
  puts("#{request["method"]} #{request["path"]} -> #{response[0]}")
  response
end

# Outermost in the chain (see app() below) so its timing covers every
# other middleware's own work too, not just route()'s.
def timing_middleware(request, context, forward)
  start = Time.monotonic()
  response = forward(request, context)
  elapsed_ms = (Time.monotonic() - start) * 1000
  rounded_ms = to_f(to_i(elapsed_ms * 100)) / 100.0
  puts("#{request["method"]} #{request["path"]} took #{rounded_ms}ms")
  response
end

def app(request, context)
  chain = rack_compose([timing_middleware, logging_middleware], route)
  rack_run_chain(chain, 0, request, context)
end

puts("listening on http://127.0.0.1:18080 (Ctrl-C to stop)")
gremlin_serve(18080, app)
