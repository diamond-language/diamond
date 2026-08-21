require "../../packages/diamond-active_record/lib/diamond-active_record"
require "../../lib/minitest"

class LibraryAuthor
  def initialize(id, name)
    @id = id
    @name = name
  end
  def id() = @id
end

class LibraryBook
  def initialize(id, title)
    @id = id
    @title = title
  end
  def title() = @title
end

def run_tests()
  def map_author(row)
    LibraryAuthor.new(row["id"], row["name"])
  end
  def map_book(row)
    LibraryBook.new(row["id"], row["title"])
  end

  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT)")
  db.execute("CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT)")
  db.execute("CREATE TABLE authorships (author_id INTEGER, book_id INTEGER)")
  db.execute("INSERT INTO authors (name) VALUES (?)", ["Ada"])
  db.execute("INSERT INTO authors (name) VALUES (?)", ["Grace"])
  db.execute("INSERT INTO books (title) VALUES (?)", ["Sketch of the Analytical Engine"])
  db.execute("INSERT INTO books (title) VALUES (?)", ["Notes on the Analytical Engine"])
  db.execute("INSERT INTO books (title) VALUES (?)", ["Compilers and Computers"])
  # Ada co-authors both her books; Grace has written the third alone but
  # shares no authorship row with Ada -- proves the join filters by owner,
  # not just by touching the join table at all.
  db.execute("INSERT INTO authorships (author_id, book_id) VALUES (?, ?)", [1, 1])
  db.execute("INSERT INTO authorships (author_id, book_id) VALUES (?, ?)", [1, 2])
  db.execute("INSERT INTO authorships (author_id, book_id) VALUES (?, ?)", [2, 3])

  authors = ActiveRecord::Repository.new(Arel.table("authors"), map_author)
  books = ActiveRecord::Repository.new(Arel.table("books"), map_book)
  author_books = ActiveRecord::HasManyThrough.new(
    books, Arel.table("authorships"), "author_id", "book_id")

  ada = authors.find(db, 1)
  ada_books = author_books.all(db, ada.id())
  Minitest.assert_equal(2, ada_books.length())
  Minitest.assert_equal("Sketch of the Analytical Engine", ada_books[0].title())
  Minitest.assert_equal("Notes on the Analytical Engine", ada_books[1].title())

  grace = authors.find(db, 2)
  grace_books = author_books.all(db, grace.id())
  Minitest.assert_equal(1, grace_books.length())
  Minitest.assert_equal("Compilers and Computers", grace_books[0].title())

  db.execute("INSERT INTO authors (name) VALUES (?)", ["Unpublished"])
  unpublished = authors.find(db, 3)
  Minitest.assert_equal(0, author_books.all(db, unpublished.id()).length())

  db.close()
end

run_tests()
puts("diamond_active_record has_many_through smoke ok")
