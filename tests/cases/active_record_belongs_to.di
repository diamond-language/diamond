require "../../packages/active_record/lib/active_record"
require "../../lib/minitest"

class LibraryAuthor
  def initialize(id, name)
    @id = id
    @name = name
  end
  def name() = @name
end

class LibraryBook
  def initialize(id, title, author_id)
    @id = id
    @title = title
    @author_id = author_id
  end
  def author_id() = @author_id
end

def run_tests()
  def map_author(row)
    LibraryAuthor.new(row["id"], row["name"])
  end
  def map_book(row)
    LibraryBook.new(row["id"], row["title"], row["author_id"])
  end

  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT)")
  db.execute("CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT, author_id INTEGER)")
  db.execute("INSERT INTO authors (name) VALUES (?)", ["Ada"])
  db.execute("INSERT INTO books (title, author_id) VALUES (?, ?)", ["Book A", 1])
  db.execute("INSERT INTO books (title, author_id) VALUES (?, ?)", ["Orphan Book", 99])

  authors = ActiveRecord::Repository.new(Arel.table("authors"), map_author)
  books = ActiveRecord::Repository.new(Arel.table("books"), map_book)
  book_author = ActiveRecord::BelongsTo.new(authors)

  book = books.find(db, 1)
  author = book_author.get(db, book.author_id())
  Minitest.assert_equal("Ada", author.name())

  orphan = books.find(db, 2)
  Minitest.assert_nil(book_author.get(db, orphan.author_id()))

  db.close()
end

run_tests()
puts("active_record belongs_to smoke ok")
