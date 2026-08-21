require "../../packages/diamond-active_record/lib/diamond-active_record"
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

def run_tests()
  def map_author(row)
    LibraryAuthor.new(row["id"], row["name"], row["country"])
  end

  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT, country TEXT)")
  db.execute("INSERT INTO authors (name, country) VALUES (?, ?)", ["Ada", "UK"])
  db.execute("INSERT INTO authors (name, country) VALUES (?, ?)", ["Grace", "USA"])

  # An explicit visitor (here ArelSQLiteVisitor itself, to stay
  # self-contained without a live PostgreSQL server -- see
  # packages/arel/test_postgres_dialect.di for the cross-dialect version
  # of this same check) should thread through every repository method
  # identically to leaving it nil.
  repository = ActiveRecordRepository.new(
    Arel.table("authors"), map_author, "id", ArelSQLiteVisitor.new())

  Minitest.assert_equal("Ada", repository.find(db, 1).name())
  Minitest.assert_equal(2, repository.all(db).length())

  conditions = {}
  conditions["country"] = "UK"
  Minitest.assert_equal(1, repository.where(db, conditions).length())

  Minitest.assert_equal(1, repository.create(db, {"name": "Marie", "country": "France"}))
  Minitest.assert_equal(3, repository.all(db).length())

  Minitest.assert_equal(1, repository.update(db, 1, {"country": "England"}))
  Minitest.assert_equal("England", repository.find(db, 1).country())

  Minitest.assert_equal(1, repository.delete(db, 3))
  Minitest.assert_equal(2, repository.all(db).length())

  db.close()
end

run_tests()
puts("diamond_active_record explicit-visitor smoke ok")
