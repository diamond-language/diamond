require "../../packages/active_record/lib/active_record"
require "../../lib/minitest"

class LibraryAuthor
  def initialize(id, name)
    @id = id
    @name = name
  end
  def id() = @id
  def name() = @name
end

def map_author(row) = LibraryAuthor.new(row["id"], row["name"])

def run_tests()
  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT)")
  index = 0
  while index < 25
    db.execute("INSERT INTO authors (name) VALUES (?)", ["Author #{index}"])
    index += 1
  end

  repository = ActiveRecord::Repository.new(Arel.table("authors"), map_author)

  # find_each: one record per callback call, every row visited exactly
  # once, in ascending id order, regardless of batch_size.
  seen_ids = []
  def collect_id(author)
    seen_ids.push(author.id())
  end
  repository.find_each(db, collect_id, 7)
  Minitest.assert_equal(25, seen_ids.length())
  Minitest.assert_equal(1, seen_ids[0])
  Minitest.assert_equal(25, seen_ids[24])

  # find_in_batches: one Array of at most batch_size mapped records per
  # callback call -- 25 rows at batch_size 10 means batches of 10, 10, 5.
  batch_sizes = []
  def collect_batch_size(batch)
    batch_sizes.push(batch.length())
  end
  repository.find_in_batches(db, collect_batch_size, 10)
  Minitest.assert_equal(3, batch_sizes.length())
  Minitest.assert_equal(10, batch_sizes[0])
  Minitest.assert_equal(10, batch_sizes[1])
  Minitest.assert_equal(5, batch_sizes[2])

  # An empty table calls the callback zero times, not once with an
  # empty batch.
  db.execute("DELETE FROM authors")
  empty_calls = []
  def record_call(batch)
    empty_calls.push(true)
  end
  repository.find_in_batches(db, record_call, 10)
  Minitest.assert_equal(0, empty_calls.length())

  # batch_size must be positive.
  message = nil
  begin
    repository.find_each(db, collect_id, 0)
  rescue error: ArgumentError
    message = error.message()
  end
  Minitest.assert_equal("batch_size must be at least 1", message)

  db.close()
end

run_tests()
puts("active_record batches smoke ok")
