require "../../packages/active_record/lib/active_record"
require "../../lib/minitest"

class LibraryAccount
  def initialize(id, balance, lock_version)
    @id = id
    @balance = balance
    @lock_version = lock_version
  end
  def id() = @id
  def balance() = @balance
  def lock_version() = @lock_version
end

def run_tests()
  def map_account(row)
    LibraryAccount.new(row["id"], row["balance"], row["lock_version"])
  end

  db = SQLite3.open(":memory:")
  db.execute(
    "CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER, lock_version INTEGER)")
  db.execute("INSERT INTO accounts (balance, lock_version) VALUES (?, ?)", [100, 0])

  repository = ActiveRecord::Repository.new(
    Arel.table("accounts"), map_account, "id", nil, nil, nil, nil, "lock_version")

  account = repository.find(db, 1)
  Minitest.assert_equal(0, account.lock_version())

  # A normal update bumps lock_version by one and succeeds.
  affected = repository.update(db, 1, {"balance": 150}, account.lock_version())
  Minitest.assert_equal(1, affected)
  reloaded = repository.find(db, 1)
  Minitest.assert_equal(150, reloaded.balance())
  Minitest.assert_equal(1, reloaded.lock_version())

  # Updating again against the now-stale version the first `account`
  # handle still holds raises, and leaves the row untouched.
  message = nil
  begin
    repository.update(db, 1, {"balance": 200}, account.lock_version())
  rescue error: ActiveRecord::StaleObjectError
    message = error.message()
  end
  Minitest.assert_equal("attempted to update a stale object (id=1)", message)
  unchanged = repository.find(db, 1)
  Minitest.assert_equal(150, unchanged.balance())
  Minitest.assert_equal(1, unchanged.lock_version())

  # Updating with the correct (current) version succeeds and bumps again.
  current = repository.find(db, 1)
  Minitest.assert_equal(1, repository.update(db, 1, {"balance": 200}, current.lock_version()))
  Minitest.assert_equal(2, repository.find(db, 1).lock_version())

  # expected_lock_version is required once lock_column is configured.
  arg_message = nil
  begin
    repository.update(db, 1, {"balance": 999})
  rescue error: ArgumentError
    arg_message = error.message()
  end
  Minitest.assert_equal(
    "expected_lock_version is required when lock_column is configured", arg_message)

  db.close()
end

run_tests()
puts("active_record optimistic_locking smoke ok")
