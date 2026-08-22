require "../../packages/active_record/lib/active_record"
require "../../lib/minitest"

class LibraryAuthor
  def initialize(id, name)
    @id = id
    @name = name
  end
  def name() = @name
end

def callback_noop() = nil

def run_tests()
  def map_author(row)
    LibraryAuthor.new(row["id"], row["name"])
  end

  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT)")
  repository = ActiveRecord::Repository.new(Arel.table("authors"), map_author)

  # A nested transaction that raises rolls back only to the savepoint --
  # the outer transaction, and anything committed before the nested
  # block, are untouched.
  ActiveRecord::Transaction.run(db) do
    repository.create(db, {"name": "Ada"})
    failed = false
    begin
      ActiveRecord::Transaction.run_nested(db, "before_grace") do
        repository.create(db, {"name": "Grace"})
        raise RuntimeError.new("oops")
      end
    rescue error: RuntimeError
      failed = true
    end
    Minitest.assert_equal(true, failed)
  end
  Minitest.assert_equal(1, repository.all(db).length())
  Minitest.assert_equal("Ada", repository.all(db)[0].name())

  # A nested transaction that returns normally keeps its own writes.
  ActiveRecord::Transaction.run(db) do
    ActiveRecord::Transaction.run_nested(db, "savepoint_marie") do
      repository.create(db, {"name": "Marie"})
    end
  end
  Minitest.assert_equal(2, repository.all(db).length())

  # Savepoint name validation.
  empty_message = nil
  begin
    ActiveRecord::Transaction.run_nested(db, "", callback_noop)
  rescue error: ArgumentError
    empty_message = error.message()
  end
  Minitest.assert_equal("savepoint name cannot be empty", empty_message)

  digit_message = nil
  begin
    ActiveRecord::Transaction.run_nested(db, "1abc", callback_noop)
  rescue error: ArgumentError
    digit_message = error.message()
  end
  Minitest.assert_equal("savepoint name cannot start with a digit: 1abc", digit_message)

  bad_char_message = nil
  begin
    ActiveRecord::Transaction.run_nested(db, "sp; DROP TABLE authors; --", callback_noop)
  rescue error: ArgumentError
    bad_char_message = error.message()
  end
  Minitest.assert_equal(
    "savepoint name must contain only letters, digits, and underscores: sp; DROP TABLE authors; --",
    bad_char_message)
  Minitest.assert_equal(2, repository.all(db).length())

  db.close()
end

run_tests()
puts("active_record nested_transaction smoke ok")
