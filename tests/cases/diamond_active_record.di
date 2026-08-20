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
  db.close()
end

run_tests()
