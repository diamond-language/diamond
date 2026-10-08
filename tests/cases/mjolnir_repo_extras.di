require "../../packages/mjolnir/lib/mjolnir"
require "../../lib/minitest"

struct XAuthor(id: Int, name: String, created_at: Int | Nil, updated_at: Int | Nil)
end
struct XBook(id: Int, author_id: Int | Nil, title: String)
end

def build_x_author(row) = XAuthor.new(row["id"], row["name"], row["created_at"], row["updated_at"])
def build_x_book(row) = XBook.new(row["id"], row["author_id"], row["title"])

def x_schemas()
  authors = Mjolnir::Schema.new("authors",
    {"id": :int, "name": :string, "created_at": :int, "updated_at": :int}, build_x_author, "id", true)
  books = Mjolnir::Schema.new("books", {"id": :int, "author_id": :int, "title": :string}, build_x_book)
  authors.has_many("books", books, "author_id")
  books.belongs_to("author", authors, "author_id")
  [authors, books]
end

def x_repo()
  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT NOT NULL, created_at INTEGER, updated_at INTEGER)")
  db.execute("CREATE TABLE books (id INTEGER PRIMARY KEY, author_id INTEGER, title TEXT NOT NULL)")
  Mjolnir::Repo.new(db)
end

def x_author(repo, authors, name)
  repo.insert!(Mjolnir::Changeset.cast(authors, nil, {"name": name}, ["name"]))
end

def x_book(repo, books, author, title)
  attrs = {"title": title, "author_id": if author == nil then nil else author.id() end}
  repo.insert!(Mjolnir::Changeset.cast(books, nil, attrs, ["title", "author_id"]))
end

def run_tests()
  schemas = x_schemas()
  authors = schemas[0]
  books = schemas[1]
  suite = Minitest.new()

  suite.test("timestamps are stamped on insert and refreshed on update") do
    repo = x_repo()
    ada = x_author(repo, authors, "Ada")
    Minitest.assert(ada.created_at() != nil)
    Minitest.assert_equal(ada.created_at(), ada.updated_at())
    forced = Mjolnir::Changeset.cast(authors, ada, {"name": "Ada L"}, ["name"]).put_change("updated_at", 5)
    Minitest.assert_equal(5, repo.update!(forced).updated_at())
    Minitest.assert_equal(ada.created_at(), repo.get(authors, ada.id()).created_at())
  end

  suite.test("a timestamped schema must declare both columns") do
    raised = false
    begin
      Mjolnir::Schema.new("t", {"id": :int}, build_x_book, "id", true)
    rescue error: ArgumentError
      raised = true
    end
    Minitest.assert(raised)
  end

  suite.test("where takes an Array as IN, including an empty one") do
    repo = x_repo()
    ids = ["a", "b", "c"].map() do |name| x_author(repo, authors, name).id() end
    two = repo.all(authors.query().where({"id": [ids[0], ids[2]]}).order_by("id"))
    Minitest.assert_equal(["a", "c"], two.map() do |author| author.name() end)
    Minitest.assert_equal(0, repo.count(authors.query().where({"id": []})))
  end

  suite.test("order_by accepts a raw expression") do
    repo = x_repo()
    ["a", "b", "c"].each() do |name| x_author(repo, authors, name) end
    Minitest.assert_equal(3, repo.all(authors.query().order_by(Arel.sql("RANDOM()"))).length())
  end

  suite.test("get_by and exists? look rows up by field values") do
    repo = x_repo()
    x_author(repo, authors, "Ada")
    Minitest.assert_equal("Ada", repo.get_by(authors, {"name": "Ada"}).name())
    Minitest.assert_equal(nil, repo.get_by(authors, {"name": "Bo"}))
    Minitest.assert(repo.exists?(authors.query().where({"name": "Ada"})))
    Minitest.assert(!repo.exists?(authors.query().where({"name": "Bo"})))
  end

  suite.test("insert! and update! raise on an invalid changeset") do
    repo = x_repo()
    raised = false
    begin
      repo.insert!(Mjolnir::Changeset.cast(authors, nil, {}, ["name"]).validate_required(["name"]))
    rescue error: Mjolnir::InvalidChangesetError
      raised = error.changeset().errors().has_key?("name")
    end
    Minitest.assert(raised)
  end

  suite.test("update_all and delete_all act on matching rows and refuse no condition") do
    repo = x_repo()
    ["a", "b", "c"].each() do |name| x_author(repo, authors, name) end
    Minitest.assert_equal(2, repo.update_all(authors.query().where({"name": ["a", "b"]}), {"name": "z"}))
    Minitest.assert_equal(2, repo.count(authors.query().where({"name": "z"})))
    Minitest.assert(repo.get_by(authors, {"name": "z"}).updated_at() != nil)
    Minitest.assert_equal(2, repo.delete_all(authors.query().where({"name": "z"})))
    Minitest.assert_equal(1, repo.count(authors.query()))
    refused = 0
    begin
      repo.delete_all(authors.query())
    rescue error: ArgumentError
      refused += 1
    end
    begin
      repo.update_all(authors.query(), {"name": "q"})
    rescue error: ArgumentError
      refused += 1
    end
    Minitest.assert_equal(2, refused)
    Minitest.assert_equal(1, repo.count(authors.query()))
  end

  suite.test("preload loads has_many in one query, empty for owners with none") do
    repo = x_repo()
    ada = x_author(repo, authors, "Ada")
    bo = x_author(repo, authors, "Bo")
    x_book(repo, books, ada, "One")
    x_book(repo, books, ada, "Two")
    loaded = repo.all(authors.query().order_by("id"))
    grouped = repo.preload(authors, loaded, "books")
    Minitest.assert_equal(["One", "Two"], grouped[ada.id()].map() do |book| book.title() end)
    Minitest.assert_equal([], grouped[bo.id()])
    scoped = repo.preload(authors, loaded, "books", books.query().where({"title": "Two"}).order_by("id", :desc))
    Minitest.assert_equal(["Two"], scoped[ada.id()].map() do |book| book.title() end)
  end

  suite.test("preload loads belongs_to, nil for a missing or null key") do
    repo = x_repo()
    ada = x_author(repo, authors, "Ada")
    owned = x_book(repo, books, ada, "One")
    orphan = x_book(repo, books, nil, "Orphan")
    found = repo.preload(books, [owned, orphan], "author")
    Minitest.assert_equal("Ada", found[owned.id()].name())
    Minitest.assert_equal(nil, found[orphan.id()])
    Minitest.assert_equal({}, repo.preload(books, [], "author"))
  end

  suite.test("an unknown association raises") do
    raised = false
    begin
      x_repo().preload(authors, [], "nope")
    rescue error: ArgumentError
      raised = true
    end
    Minitest.assert(raised)
  end

  suite.run!()
end

run_tests()
