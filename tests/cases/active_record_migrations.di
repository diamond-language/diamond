# ActiveRecord::Migrator: versioned, ordered schema changes (see
# packages/active_record/README.md's "Migrations" section). A migration
# is a plain Hash ("version"/"up"/"down"), not a class -- ClassName.method
# calls only ever resolve against a literal class name at compile time in
# Diamond, so a class-shaped migration could never be invoked dynamically
# from an Array the way Migrator needs (see migration.di's own comment).
require "../../packages/active_record/lib/active_record"
require "../../lib/minitest"

def create_authors_up(db)
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT)")
end
def create_authors_down(db)
  db.execute("DROP TABLE authors")
end
def create_authors_migration() = {
  "version": "20260101000001", "up": create_authors_up, "down": create_authors_down
}

def create_books_up(db)
  db.execute("CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT)")
end
# Deliberately no "down" entry -- exercises the "irreversible migration"
# error path below.
def create_books_migration() = {"version": "20260101000002", "up": create_books_up}

def run_tests()
  def test_run_applies_every_migration_in_order()
    db = SQLite3.open(":memory:")
    migrations = [create_authors_migration(), create_books_migration()]
    ActiveRecord::Migrator.run(db, migrations)
    Minitest.assert_equal(2, ActiveRecord::Migrator.applied(db, migrations).length())
    Minitest.assert_equal(0, ActiveRecord::Migrator.pending(db, migrations).length())
    db.execute("INSERT INTO authors (name) VALUES (?)", ["Ada"])
    db.execute("INSERT INTO books (title) VALUES (?)", ["Poems"])
    Minitest.assert_equal(1, db.query("SELECT * FROM authors").length())
    Minitest.assert_equal(1, db.query("SELECT * FROM books").length())
    db.close()
  end

  def test_run_is_idempotent()
    db = SQLite3.open(":memory:")
    migrations = [create_authors_migration()]
    ActiveRecord::Migrator.run(db, migrations)
    ActiveRecord::Migrator.run(db, migrations)
    Minitest.assert_equal(1, ActiveRecord::Migrator.applied(db, migrations).length())
    db.close()
  end

  def test_run_applies_only_newly_added_migrations()
    db = SQLite3.open(":memory:")
    ActiveRecord::Migrator.run(db, [create_authors_migration()])
    ActiveRecord::Migrator.run(db, [create_authors_migration(), create_books_migration()])
    Minitest.assert_equal(2,
      ActiveRecord::Migrator.applied(db, [create_authors_migration(), create_books_migration()]).length())
    db.close()
  end

  def test_run_rejects_duplicate_versions()
    db = SQLite3.open(":memory:")
    caught = false
    begin
      ActiveRecord::Migrator.run(db, [create_authors_migration(), create_authors_migration()])
    rescue error: ArgumentError
      caught = true
      Minitest.assert_equal(true, error.message().include?("duplicate migration version"))
    end
    Minitest.assert_equal(true, caught)
    db.close()
  end

  def test_run_rolls_back_the_failed_migration_only()
    db = SQLite3.open(":memory:")
    def broken_up(db)
      db.execute("CREATE TABLE nope (")
    end
    def broken_migration() = {"version": "20260101000003", "up": broken_up}
    caught = false
    begin
      ActiveRecord::Migrator.run(db, [create_authors_migration(), broken_migration()])
    rescue error: StandardError
      caught = true
    end
    Minitest.assert_equal(true, caught)
    # authors' own migration committed successfully before the broken one;
    # the broken one's own failed CREATE TABLE was rolled back, not applied.
    Minitest.assert_equal(true, ActiveRecord::Migrator.applied(db, [create_authors_migration()]).length() == 1)
    Minitest.assert_equal(0, ActiveRecord::Migrator.applied(db, [broken_migration()]).length())
    db.close()
  end

  def test_rollback_reverses_the_most_recent_migration()
    db = SQLite3.open(":memory:")
    migrations = [create_authors_migration()]
    ActiveRecord::Migrator.run(db, migrations)
    ActiveRecord::Migrator.rollback(db, migrations, 1)
    Minitest.assert_equal(0, ActiveRecord::Migrator.applied(db, migrations).length())
    caught = false
    begin
      db.execute("INSERT INTO authors (name) VALUES (?)", ["Ada"])
    rescue error: StandardError
      caught = true
    end
    Minitest.assert_equal(true, caught)
    db.close()
  end

  def test_rollback_raises_for_an_irreversible_migration()
    db = SQLite3.open(":memory:")
    migrations = [create_books_migration()]
    ActiveRecord::Migrator.run(db, migrations)
    caught = false
    begin
      ActiveRecord::Migrator.rollback(db, migrations, 1)
    rescue error: RuntimeError
      caught = true
      Minitest.assert_equal(true, error.message().include?("irreversible"))
    end
    Minitest.assert_equal(true, caught)
    # the schema_migrations row is untouched -- still counts as applied.
    Minitest.assert_equal(1, ActiveRecord::Migrator.applied(db, migrations).length())
    db.close()
  end

  suite = Minitest.new()
  suite.test("run applies every migration in order") do
    test_run_applies_every_migration_in_order()
  end
  suite.test("run is idempotent") do
    test_run_is_idempotent()
  end
  suite.test("run applies only newly added migrations") do
    test_run_applies_only_newly_added_migrations()
  end
  suite.test("run rejects duplicate versions") do
    test_run_rejects_duplicate_versions()
  end
  suite.test("run rolls back only the failed migration") do
    test_run_rolls_back_the_failed_migration_only()
  end
  suite.test("rollback reverses the most recent migration") do
    test_rollback_reverses_the_most_recent_migration()
  end
  suite.test("rollback raises for an irreversible migration") do
    test_rollback_raises_for_an_irreversible_migration()
  end
  suite.run!()
end

run_tests()
