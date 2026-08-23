# End-to-end demo: a gremlin server, wrapped in a rack logging/timing
# middleware chain, serving HTML pages backed by ActiveRecord::Model
# classes (Author, Book) over arel queries against the SQLite3 database
# setup_db.di seeded. Run setup_db.di first.
#
# Organized as plain Diamond classes throughout: Author/Book are
# ActiveRecord::Model subclasses with a real has_many/belongs_to
# association between them; Page/TableView/Form/Response hold view-rendering
# and response-building as namespaced `self.` methods instead of free
# top-level functions; AuthorsController/BooksController hold one
# `self.` method per route action; Router owns path parsing and
# dispatches to them.
#
# The one place this app is still deliberately plain functions is the
# rack middleware chain (`route`/`logging_middleware`/
# `timing_middleware`/`app` below) -- not a style choice but a real
# constraint documented in packages/rack/rack.di: a middleware is a
# `Callable`, and Diamond classes are compile-time metadata, not
# first-class runtime values (docs/roadmap.md's "Classes as ordinary
# runtime objects" section) -- there is no way to hand `rack_compose` a
# class or an instance in place of a bare, zero-capture `def` reference.
# `route` is a one-line shim into `Router.dispatch`, the same pattern
# rack's own `rack_terminal_wrap` uses internally for an app handler.
#
# gremlin_serve gives each worker its own persistent `context` Hash
# (see packages/gremlin/gremlin.di's "Per-worker context") -- the db
# connection is opened lazily on this worker's first request and
# stashed there, reused by every request after (Database.get below).

require "../../packages/active_record/lib/active_record"
require "../../packages/gremlin/lib/gremlin"
require "../../packages/rack/lib/rack"

# Per-worker SQLite connection, lazily opened and cached on `context` --
# same one-instance-per-worker shape RackChain (packages/rack/rack.di)
# uses for the middleware chain itself.
class Database
  def self.path() = "library.db"
  def self.get(context)
    db = context["db"]
    if db == nil
      db = SQLite3.open(Database.path())
      context["db"] = db
    end
    db
  end
end

class Author < ActiveRecord::Model
  attr_accessor name: String, country: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @name = attributes["name"]
    @country = attributes["country"]
  end

  def to_attributes() = {"name": @name, "country": @country}
  def repository() = @@repository

  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def books(db) = self.has_many(Book.repository(), "author_id").all(db, self.id())
end

def build_author(row) = Author.new(row)

class Book < ActiveRecord::Model
  attr_accessor title: String, author_id, year, available

  def initialize(attributes: Hash = {})
    super(attributes)
    @title = attributes["title"]
    @author_id = attributes["author_id"]
    @year = attributes["year"]
    @available = attributes["available"]
  end

  def to_attributes()
    {"title": @title, "author_id": @author_id, "year": @year, "available": @available}
  end
  def repository() = @@repository

  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def available?() -> Bool = @available == 1
  def author(db) = self.belongs_to(Author.repository()).get(db, @author_id)
end

def build_book(row) = Book.new(row)

# ---------------------------------------------------------------------
# Views: HTML rendering, grouped by concern rather than as free
# functions closing over a mutable `rows` local the way this file used
# to build tables (`Array#map` + `#join` replaces that pattern here).
# ---------------------------------------------------------------------

class Page
  def self.nav()
    "<p><a href=\"/\">Home</a> | <a href=\"/authors\">Authors</a> | " +
    "<a href=\"/books\">Books</a> | " +
    "<a href=\"/books/available\">Available books</a> | " +
    "<a href=\"/authors/new\">New author</a> | " +
    "<a href=\"/books/new\">New book</a></p>"
  end

  def self.render(title, body)
    "<!DOCTYPE html><html><head><title>#{title}</title></head>" +
    "<body><h1>#{title}</h1>#{Page.nav()}#{body}</body></html>"
  end
end

class TableView
  def self.authors(authors: Array)
    rows = authors.map() do |author|
      "<tr><td><a href=\"/authors/#{author.id()}\">#{author.name()}</a></td>" +
        "<td>#{author.country()}</td></tr>"
    end.join()
    "<table border=\"1\"><tr><th>Name</th><th>Country</th></tr>#{rows}</table>"
  end

  # `authors_by_id`: Hash of author id -> Author, from a batch
  # BelongsTo#preload (see BooksController.authors_by_id) so this
  # doesn't run one query per row.
  def self.books(books: Array, authors_by_id: Hash)
    rows = books.map() do |book|
      author = authors_by_id[book.author_id()]
      author_name = if author == nil then "" else author.name() end
      status = if book.available?() then "yes" else "no" end
      "<tr><td><a href=\"/books/#{book.id()}\">#{book.title()}</a></td>" +
        "<td>#{author_name}</td><td>#{book.year()}</td><td>#{status}</td></tr>"
    end.join()
    "<table border=\"1\"><tr><th>Title</th><th>Author</th><th>Year</th>" +
    "<th>Available</th></tr>#{rows}</table>"
  end

  # Books already scoped to one author (their own show page) -- no
  # author column needed.
  def self.author_books(books: Array)
    rows = books.map() do |book|
      status = if book.available?() then "yes" else "no" end
      "<tr><td><a href=\"/books/#{book.id()}\">#{book.title()}</a></td>" +
        "<td>#{book.year()}</td><td>#{status}</td></tr>"
    end.join()
    "<table border=\"1\"><tr><th>Title</th><th>Year</th><th>Available</th></tr>#{rows}</table>"
  end
end

class Form
  def self.decode(value: String) -> String
    result = value
    plus = result.index_of("+")
    while plus != nil
      result = result.slice(0, plus) + " " + result.slice(plus + 1, result.length())
      plus = result.index_of("+")
    end
    result
  end

  def self.parse(request)
    values = {}
    source = request["body"]
    if request["method"] == "GET"
      source = request["path"]
      question = source.index_of("?")
      source = if question == nil then "" else source.slice(question + 1, source.length()) end
    end
    source.split("&").each() do |pair|
      equals = pair.index_of("=")
      if equals != nil
        values[Form.decode(pair.slice(0, equals))] = Form.decode(pair.slice(equals + 1, pair.length()))
      end
    end
    values
  end

  def self.render(action, method, fields: Array, submit)
    rows = fields.map() do |field|
      "<p><label>#{field["label"]}: " +
        "<input name=\"#{field["name"]}\" value=\"#{field["value"]}\"></label></p>"
    end.join()
    "<form method=\"#{method}\" action=\"#{action}\">#{rows}" +
      "<p><button type=\"submit\">#{submit}</button></p></form>"
  end
end

class Response
  def self.html(status: Int, body) = [status, {"Content-Type": "text/html"}, body]
  def self.text(status: Int, body) = [status, {"Content-Type": "text/plain"}, body]
  def self.redirect(location: String, body) = [302, {"Location": location}, body]
  def self.not_found(path) = Response.text(404, "not found: #{path}")
end

# ---------------------------------------------------------------------
# Controllers: one `self.` method per route action.
# ---------------------------------------------------------------------

class AuthorsController
  def self.index(request, context)
    db = Database.get(context)
    authors = Author.all().order("name").to_a(db)
    Response.html(200, Page.render("Authors", TableView.authors(authors)))
  end

  def self.show(request, context, id)
    db = Database.get(context)
    author = Author.find(db, id)
    if author == nil
      return Response.not_found(request["path"])
    end
    body = "<p>#{author.name()} is from #{author.country()}.</p>" +
      "<h2>Books</h2>" + TableView.author_books(author.books(db)) +
      "<p><a href=\"/authors/#{id}/edit\">Edit author</a> | " +
      "<form method=\"POST\" action=\"/authors/#{id}/delete\"><button>Delete author</button></form></p>"
    Response.html(200, Page.render(author.name(), body))
  end

  # Diamond's declaration-discovery pass lets one class forward-reference
  # *another* class's not-yet-compiled methods, but a class's own real
  # compile pass rebuilds its method table top to bottom as it goes
  # (see docs/roadmap.md's "Compiler representation" section) -- so
  # `.form` below has to come before new_form/edit, its own callers,
  # even though it's private helper-shaped and would otherwise read
  # better lower down.
  def self.form(request, context, id)
    db = Database.get(context)
    author = if id == nil then Author.new({"name": "", "country": ""}) else Author.find(db, id) end
    if id != nil && author == nil
      return Response.not_found(request["path"])
    end
    action = if id == nil then "/authors" else "/authors/#{id}" end
    title = if id == nil then "New author" else "Edit author" end
    submit = if id == nil then "Create author" else "Save author" end
    body = Form.render(action, "POST", [
      {"name": "name", "label": "Name", "value": author.name()},
      {"name": "country", "label": "Country", "value": author.country()}
    ], submit)
    Response.html(200, Page.render(title, body))
  end

  def self.new_form(request, context) = AuthorsController.form(request, context, nil)
  def self.edit(request, context, id) = AuthorsController.form(request, context, id)

  def self.create(request, context)
    db = Database.get(context)
    form = Form.parse(request)
    Author.create(db, {"name": form["name"], "country": form["country"]})
    Response.redirect("/authors", "created")
  end

  def self.update(request, context, id)
    db = Database.get(context)
    author = Author.find(db, id)
    if author == nil
      return Response.not_found(request["path"])
    end
    form = Form.parse(request)
    author.name = form["name"]
    author.country = form["country"]
    author.save(db)
    Response.redirect("/authors/#{id}", "updated")
  end

  def self.destroy(request, context, id)
    db = Database.get(context)
    author = Author.find(db, id)
    if author != nil
      author.destroy(db)
    end
    Response.redirect("/authors", "deleted")
  end
end

class BooksController
  # Batch-loads the authors a page of books references, one query
  # instead of one per row -- ActiveRecord::BelongsTo#preload, see
  # packages/active_record/README.md's "N+1" section.
  def self.authors_by_id(db, books: Array)
    author_ids = books.map() do |book| book.author_id() end
    ActiveRecord::BelongsTo.new(Author.repository()).preload(db, author_ids)
  end

  def self.index(request, context)
    db = Database.get(context)
    books = Book.all().order("year").to_a(db)
    Response.html(200, Page.render("Books",
      TableView.books(books, BooksController.authors_by_id(db, books))))
  end

  def self.available(request, context)
    db = Database.get(context)
    books = Book.where({"available": 1}).order("year").to_a(db)
    Response.html(200, Page.render("Available books",
      TableView.books(books, BooksController.authors_by_id(db, books))))
  end

  def self.show(request, context, id)
    db = Database.get(context)
    book = Book.find(db, id)
    if book == nil
      return Response.not_found(request["path"])
    end
    # book.author(db) can be nil -- there's no cascading delete here (see
    # AuthorsController.destroy), so a book can outlive its author.
    author = book.author(db)
    author_link = if author == nil
      "unknown"
    else
      "<a href=\"/authors/#{book.author_id()}\">#{author.name()}</a>"
    end
    body = "<p>Author: #{author_link}</p>" +
      "<p>Published: #{book.year()}</p><p>Available: #{book.available()}</p>" +
      "<p><a href=\"/books/#{id}/edit\">Edit book</a> | " +
      "<form method=\"POST\" action=\"/books/#{id}/delete\"><button>Delete book</button></form></p>"
    Response.html(200, Page.render(book.title(), body))
  end

  # See AuthorsController.form's own comment on why this has to come
  # before new_form/edit, its own callers.
  def self.form(request, context, id)
    db = Database.get(context)
    book = if id == nil
      Book.new({"title": "", "author_id": "", "year": "", "available": 1})
    else
      Book.find(db, id)
    end
    if id != nil && book == nil
      return Response.not_found(request["path"])
    end
    authors = Author.all().order("name").to_a(db)
    options = authors.map() do |author|
      selected = if author.id() == book.author_id() then " selected" else "" end
      "<option value=\"#{author.id()}\"#{selected}>#{author.name()}</option>"
    end.join()
    action = if id == nil then "/books" else "/books/#{id}" end
    title = if id == nil then "New book" else "Edit book" end
    submit = if id == nil then "Create book" else "Save book" end
    body = "<form method=\"POST\" action=\"#{action}\">" +
      "<p><label>Author: <select name=\"author_id\">#{options}</select></label></p>" +
      "<p><label>Title: <input name=\"title\" value=\"#{book.title()}\"></label></p>" +
      "<p><label>Year: <input name=\"year\" value=\"#{book.year()}\"></label></p>" +
      "<p><label>Available (1 or 0): <input name=\"available\" value=\"#{book.available()}\"></label></p>" +
      "<p><button type=\"submit\">#{submit}</button></p></form>"
    Response.html(200, Page.render(title, body))
  end

  def self.new_form(request, context) = BooksController.form(request, context, nil)
  def self.edit(request, context, id) = BooksController.form(request, context, id)

  def self.attributes_from_form(form: Hash)
    {"title": form["title"], "author_id": form["author_id"].to_i(),
     "year": form["year"].to_i(), "available": form["available"].to_i()}
  end

  def self.create(request, context)
    db = Database.get(context)
    Book.create(db, BooksController.attributes_from_form(Form.parse(request)))
    Response.redirect("/books", "created")
  end

  def self.update(request, context, id)
    db = Database.get(context)
    book = Book.find(db, id)
    if book == nil
      return Response.not_found(request["path"])
    end
    values = BooksController.attributes_from_form(Form.parse(request))
    book.title = values["title"]
    book.author_id = values["author_id"]
    book.year = values["year"]
    book.available = values["available"]
    book.save(db)
    Response.redirect("/books/#{id}", "updated")
  end

  def self.destroy(request, context, id)
    db = Database.get(context)
    book = Book.find(db, id)
    if book != nil
      book.destroy(db)
    end
    Response.redirect("/books", "deleted")
  end
end

# ---------------------------------------------------------------------
# Routing.
# ---------------------------------------------------------------------

class Router
  def self.path_without_query(path)
    question = path.index_of("?")
    if question == nil then path else path.slice(0, question) end
  end

  # "42/edit" -> [42, "/edit"]; "42" -> [42, ""].
  def self.segment_id(rest: String)
    slash = rest.index_of("/")
    id_text = if slash == nil then rest else rest.slice(0, slash) end
    suffix = if slash == nil then "" else rest.slice(slash, rest.length()) end
    [id_text.to_i(), suffix]
  end

  def self.dispatch(request, context)
    path = Router.path_without_query(request["path"])
    if path == "/"
      Response.html(200, Page.render("Library",
        "<p>A tiny end-to-end demo: SQLite3 + arel + active_record + gremlin + rack.</p>"))
    elsif path == "/authors"
      if request["method"] == "POST"
        AuthorsController.create(request, context)
      else
        AuthorsController.index(request, context)
      end
    elsif path == "/authors/new"
      AuthorsController.new_form(request, context)
    elsif path == "/books"
      if request["method"] == "POST"
        BooksController.create(request, context)
      else
        BooksController.index(request, context)
      end
    elsif path == "/books/new"
      BooksController.new_form(request, context)
    elsif path == "/books/available"
      BooksController.available(request, context)
    elsif path.slice(0, 9) == "/authors/"
      id, suffix = Router.segment_id(path.slice(9, path.length()))
      if suffix == "/edit"
        AuthorsController.edit(request, context, id)
      elsif suffix == "/delete"
        AuthorsController.destroy(request, context, id)
      elsif request["method"] == "POST"
        AuthorsController.update(request, context, id)
      else
        AuthorsController.show(request, context, id)
      end
    elsif path.slice(0, 7) == "/books/"
      id, suffix = Router.segment_id(path.slice(7, path.length()))
      if suffix == "/edit"
        BooksController.edit(request, context, id)
      elsif suffix == "/delete"
        BooksController.destroy(request, context, id)
      elsif request["method"] == "POST"
        BooksController.update(request, context, id)
      else
        BooksController.show(request, context, id)
      end
    else
      Response.not_found(request["path"])
    end
  end
end

# ---------------------------------------------------------------------
# rack/gremlin wiring -- plain functions from here down; see the file
# header for why (rack middleware and gremlin_serve's handler must be
# zero-capture Callables, and classes aren't first-class values here).
# ---------------------------------------------------------------------

def route(request, context) = Router.dispatch(request, context)

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

Author.configure(ActiveRecord::Repository.new(Arel.table("authors"), build_author, "id"))
Book.configure(ActiveRecord::Repository.new(Arel.table("books"), build_book, "id"))

puts("listening on http://127.0.0.1:18080 (Ctrl-C to stop)")
gremlin_serve(18080, app)
