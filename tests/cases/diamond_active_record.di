require "../../packages/diamond-active_record/diamond-active_record"
require "../../lib/minitest"

class LibraryAuthor
  def initialize(id, name, country)
    @id = id
    @name = name
    @country = country
  end
  def name() = @name
  def country() = @country
end

class LibraryBook
  def initialize(title)
    @title = title
  end
  def title() = @title
end

def run_tests()
  def map_author(row)
    LibraryAuthor.new(row["id"], row["name"], row["country"])
  end

  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT, country TEXT)")
  db.execute("INSERT INTO authors (name, country) VALUES (?, ?)", ["Ada", "UK"])
  repository = ActiveRecordRepository.new(Arel.table("authors"), map_author)
  author = repository.find(db, 1)
  Minitest.assert_equal("Ada", author.name())
  Minitest.assert_equal(1, repository.create(db, {"name": "Grace", "country": "USA"}))
  Minitest.assert_equal(2, repository.all(db).length())
  Minitest.assert_equal(1, repository.update(db, 1, {"country": "England"}))
  Minitest.assert_equal("England", repository.find(db, 1).country())
  Minitest.assert_equal(1, repository.delete(db, 2))
  Minitest.assert_equal(1, repository.all(db).length())

  def map_book(row)
    LibraryBook.new(row["title"])
  end
  db.execute("CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT, author_id INTEGER)")
  db.execute("INSERT INTO books (title, author_id) VALUES ('Book A', 1), ('Book B', 2)")
  books = ActiveRecordRepository.new(Arel.table("books"), map_book)
  author_books = ActiveRecordHasMany.new(books, "author_id")
  Minitest.assert_equal(1, author_books.all(db, 1).length())
  Minitest.assert_equal("Book A", author_books.all(db, 1)[0].title())
  db.close()
end

run_tests()
