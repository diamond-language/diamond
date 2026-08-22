require "../../packages/active_record/lib/active_record"
require "../../lib/minitest"

class LibraryBook
  def initialize(id, title, author_id, available)
    @id = id
    @title = title
    @author_id = author_id
    @available = available
  end
  def title() = @title
end

def run_tests()
  def map_book(row)
    LibraryBook.new(row["id"], row["title"], row["author_id"], row["available"])
  end

  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT, author_id INTEGER, available INTEGER)")
  db.execute("INSERT INTO books (title, author_id, available) VALUES (?, ?, ?)", ["Book A", 1, 1])
  db.execute("INSERT INTO books (title, author_id, available) VALUES (?, ?, ?)", ["Book B", 1, 0])
  db.execute("INSERT INTO books (title, author_id, available) VALUES (?, ?, ?)", ["Book C", 2, 1])

  repository = ActiveRecord::Repository.new(Arel.table("books"), map_book)

  multi_conditions = {}
  multi_conditions["author_id"] = 1
  multi_conditions["available"] = 1
  multi_results = repository.where(db, multi_conditions)
  Minitest.assert_equal(1, multi_results.length())
  Minitest.assert_equal("Book A", multi_results[0].title())

  Minitest.assert_equal(3, repository.where(db, {}).length())

  ActiveRecord::Transaction.run(db) do
    repository.create(db, {"title": "Book D", "author_id": 3, "available": 1})
  end
  Minitest.assert_equal(4, repository.all(db).length())

  begin
    ActiveRecord::Transaction.run(db) do
      repository.create(db, {"title": "Book E", "author_id": 4, "available": 1})
      raise RuntimeError.new("boom")
    end
    Minitest.assert(false, "transaction should have raised")
  rescue e: RuntimeError
    Minitest.assert_equal("boom", e.message())
  end
  Minitest.assert_equal(4, repository.all(db).length())

  db.close()
end

run_tests()
puts("active_record where/transaction smoke ok")
