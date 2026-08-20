# End-to-end demo: a gremlin server, wrapped in a rack logging
# middleware, serving HTML pages backed by arel queries against the
# SQLite3 database setup_db.di seeded. Run setup_db.di first.
#
# gremlin_serve gives each worker its own persistent `context` Hash
# (see packages/gremlin/gremlin.di's "Per-worker context") -- the db
# connection is opened lazily on this worker's first request and
# stashed there, reused by every request after.

require "../../packages/arel/lib/arel"
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
  "<a href=\"/books/available\">Available books</a> | " +
  "<a href=\"/authors/new\">New author</a> | " +
  "<a href=\"/books/new\">New book</a></p>"
end

def render_page(title, body)
  "<!DOCTYPE html><html><head><title>#{title}</title></head>" +
  "<body><h1>#{title}</h1>#{nav()}#{body}</body></html>"
end

def decode_form_value(value)
  result = value
  plus = result.index_of("+")
  while plus != nil
    result = result.slice(0, plus) + " " + result.slice(plus + 1, result.length())
    plus = result.index_of("+")
  end
  result
end

def parse_form(request)
  values = {}
  source = request["body"]
  if request["method"] == "GET"
    source = request["path"]
    question = source.index_of("?")
    if question == nil
      source = ""
    else
      source = source.slice(question + 1, source.length())
    end
  end
  pairs = source.split("&")
  def parse_pair(pair)
    equals = pair.index_of("=")
    if equals != nil
      values[decode_form_value(pair.slice(0, equals))] = decode_form_value(pair.slice(equals + 1, pair.length()))
    end
  end
  pairs.each(parse_pair)
  values
end

def path_without_query(path)
  question = path.index_of("?")
  if question == nil then path else path.slice(0, question) end
end

def render_form(action, method, fields, submit)
  rows = ""
  def render_field(field)
    rows = rows + "<p><label>#{field["label"]}: " +
      "<input name=\"#{field["name"]}\" value=\"#{field["value"]}\"></label></p>"
  end
  fields.each(render_field)
  "<form method=\"#{method}\" action=\"#{action}\">#{rows}" +
    "<p><button type=\"submit\">#{submit}</button></p></form>"
end

def render_authors_table(authors)
  rows = ""
  def render_row(author)
    rows = rows + "<tr><td><a href=\"/authors/#{author["id"]}\">#{author["name"]}</a></td>" +
      "<td>#{author["country"]}</td></tr>"
  end
  authors.each(render_row)
  "<table border=\"1\"><tr><th>Name</th><th>Country</th></tr>#{rows}</table>"
end

def render_books_table(books)
  rows = ""
  def render_row(book)
    status = if book["available"] == 1 then "yes" else "no" end
    rows = rows + "<tr><td><a href=\"/books/#{book["id"]}\">#{book["title"]}</a></td><td>#{book["author_name"]}</td>" +
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

def books_query()
  books = Arel.table("books")
  authors = Arel.table("authors")
  Arel.from(books).project([
    books.column("id"), books.column("title"), books.column("author_id"),
    books.column("year"), books.column("available"),
    authors.column("name").as("author_name")
  ]).join(authors, books.column("author_id").eq(authors.column("id")))
end

def author_by_id(db, id)
  authors = Arel.table("authors")
  rows = Arel.from(authors).where(authors.column("id").eq(id)).take(1).to_a(db)
  if rows.length() == 0 then nil else rows[0] end
end

def books_for_author(db, author_id)
  books = Arel.table("books")
  Arel.from(books).where(books.column("author_id").eq(author_id)).order("year").to_a(db)
end

def not_found_handler(request, context)
  [404, {"Content-Type": "text/plain"}, "not found: #{request["path"]}"]
end

def render_books_table_with_links(books)
  rows = ""
  def render_row(book)
    status = if book["available"] == 1 then "yes" else "no" end
    rows = rows + "<tr><td><a href=\"/books/#{book["id"]}\">#{book["title"]}</a></td>" +
      "<td>#{book["year"]}</td><td>#{status}</td></tr>"
  end
  books.each(render_row)
  "<table border=\"1\"><tr><th>Title</th><th>Year</th><th>Available</th></tr>#{rows}</table>"
end

def author_show_handler(request, context, id)
  db = get_db(context)
  author = author_by_id(db, id)
  if author == nil
    return not_found_handler(request, context)
  end
  books = books_for_author(db, id)
  body = "<p>#{author["name"]} is from #{author["country"]}.</p>" +
    "<h2>Books</h2>" + render_books_table_with_links(books) +
    "<p><a href=\"/authors/#{id}/edit\">Edit author</a> | " +
    "<form method=\"POST\" action=\"/authors/#{id}/delete\"><button>Delete author</button></form></p>"
  [200, {"Content-Type": "text/html"}, render_page(author["name"], body)]
end

def book_by_id(db, id)
  query = books_query()
  rows = query.where(query.joins()[0].table().column("id").eq(id)).take(1).to_a(db)
  if rows.length() == 0 then nil else rows[0] end
end

def book_show_handler(request, context, id)
  db = get_db(context)
  book = book_by_id(db, id)
  if book == nil
    return not_found_handler(request, context)
  end
  body = "<p>Author: <a href=\"/authors/#{book["author_id"]}\">#{book["author_name"]}</a></p>" +
    "<p>Published: #{book["year"]}</p><p>Available: #{book["available"]}</p>" +
    "<p><a href=\"/books/#{id}/edit\">Edit book</a> | " +
    "<form method=\"POST\" action=\"/books/#{id}/delete\"><button>Delete book</button></form></p>"
  [200, {"Content-Type": "text/html"}, render_page(book["title"], body)]
end

def author_form_handler(request, context, id = nil)
  db = get_db(context)
  author = if id == nil then {"name": "", "country": ""} else author_by_id(db, id) end
  if id != nil && author == nil
    return not_found_handler(request, context)
  end
  action = if id == nil then "/authors" else "/authors/#{id}" end
  title = if id == nil then "New author" else "Edit author" end
  body = render_form(action, "POST", [
    {"name": "name", "label": "Name", "value": author["name"]},
    {"name": "country", "label": "Country", "value": author["country"]}
  ], if id == nil then "Create author" else "Save author" end)
  [200, {"Content-Type": "text/html"}, render_page(title, body)]
end

def author_write_handler(request, context, id = nil)
  db = get_db(context)
  form = parse_form(request)
  if id == nil
    Arel.insert_into(Arel.table("authors")).values({
      "name": form["name"], "country": form["country"]}).execute(db)
    return [302, {"Location": "/authors"}, "created"]
  end
  author = Arel.table("authors")
  update = Arel.update(author).set({"name": form["name"], "country": form["country"]})
  update.where(author.column("id").eq(id)).execute(db)
  [302, {"Location": "/authors/#{id}"}, "updated"]
end

def author_delete_handler(request, context, id)
  db = get_db(context)
  authors = Arel.table("authors")
  Arel.delete_from(authors).where(authors.column("id").eq(id)).execute(db)
  [302, {"Location": "/authors"}, "deleted"]
end

def book_form_handler(request, context, id = nil)
  db = get_db(context)
  book = if id == nil then {"title": "", "author_id": "", "year": "", "available": 1} else book_by_id(db, id) end
  if id != nil && book == nil
    return not_found_handler(request, context)
  end
  authors = Arel.from("authors").order("name").to_a(db)
  options = ""
  def render_author_option(author)
    selected = if author["id"] == book["author_id"] then " selected" else "" end
    options = options + "<option value=\"#{author["id"]}\"#{selected}>#{author["name"]}</option>"
  end
  authors.each(render_author_option)
  action = if id == nil then "/books" else "/books/#{id}" end
  title = if id == nil then "New book" else "Edit book" end
  submit = if id == nil then "Create book" else "Save book" end
  body = "<form method=\"POST\" action=\"#{action}\">" +
    "<p><label>Author: <select name=\"author_id\">#{options}</select></label></p>" +
    "<p><label>Title: <input name=\"title\" value=\"#{book["title"]}\"></label></p>" +
    "<p><label>Year: <input name=\"year\" value=\"#{book["year"]}\"></label></p>" +
    "<p><label>Available (1 or 0): <input name=\"available\" value=\"#{book["available"]}\"></label></p>" +
    "<p><button type=\"submit\">#{submit}</button></p></form>"
  [200, {"Content-Type": "text/html"}, render_page(title, body)]
end

def book_write_handler(request, context, id = nil)
  db = get_db(context)
  form = parse_form(request)
  values = {"title": form["title"], "author_id": form["author_id"].to_i(),
    "year": form["year"].to_i(), "available": form["available"].to_i()}
  books = Arel.table("books")
  if id == nil
    Arel.insert_into(books).values(values).execute(db)
    return [302, {"Location": "/books"}, "created"]
  end
  Arel.update(books).set(values).where(books.column("id").eq(id)).execute(db)
  [302, {"Location": "/books/#{id}"}, "updated"]
end

def book_delete_handler(request, context, id)
  db = get_db(context)
  books = Arel.table("books")
  Arel.delete_from(books).where(books.column("id").eq(id)).execute(db)
  [302, {"Location": "/books"}, "deleted"]
end

def books_handler(request, context)
  db = get_db(context)
  books = books_query().order("year").to_a(db)
  body = render_books_table(books)
  [200, {"Content-Type": "text/html"}, render_page("Books", body)]
end

def available_books_handler(request, context)
  db = get_db(context)
  books = books_query().where({"available": 1}).order("year").to_a(db)
  body = render_books_table(books)
  [200, {"Content-Type": "text/html"}, render_page("Available books", body)]
end

def route(request, context)
  path = path_without_query(request["path"])
  if path == "/"
    home_handler(request, context)
  elsif path == "/authors"
    if request["method"] == "POST" then author_write_handler(request, context) else authors_handler(request, context) end
  elsif path == "/authors/new"
    author_form_handler(request, context)
  elsif path == "/books"
    if request["method"] == "POST" then book_write_handler(request, context) else books_handler(request, context) end
  elsif path == "/books/new"
    book_form_handler(request, context)
  elsif path == "/books/available"
    available_books_handler(request, context)
  elsif path.slice(0, 9) == "/authors/"
    rest = path.slice(9, path.length())
    slash = rest.index_of("/")
    id_text = if slash == nil then rest else rest.slice(0, slash) end
    id = id_text.to_i()
    suffix = if slash == nil then "" else rest.slice(slash, rest.length()) end
    if suffix == "/edit"
      author_form_handler(request, context, id)
    elsif suffix == "/delete"
      author_delete_handler(request, context, id)
    elsif request["method"] == "POST"
      author_write_handler(request, context, id)
    else
      author_show_handler(request, context, id)
    end
  elsif path.slice(0, 7) == "/books/"
    rest = path.slice(7, path.length())
    slash = rest.index_of("/")
    id_text = if slash == nil then rest else rest.slice(0, slash) end
    id = id_text.to_i()
    suffix = if slash == nil then "" else rest.slice(slash, rest.length()) end
    if suffix == "/edit"
      book_form_handler(request, context, id)
    elsif suffix == "/delete"
      book_delete_handler(request, context, id)
    elsif request["method"] == "POST"
      book_write_handler(request, context, id)
    else
      book_show_handler(request, context, id)
    end
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
