# Opt-in cross-dialect conformance test for ActiveRecord::Repository's
# visitor parameter (see README.md). Requires an already-running PostgreSQL
# server -- run via test_postgres.sh, which manages its own throwaway
# container, the same pattern packages/arel/test_postgres_dialect.di/.sh
# already established. Deliberately outside tests/cases/, which is
# self-contained (in-memory SQLite) throughout.
require "./lib/diamond-active_record"
require "../../lib/minitest"

class LibraryAuthor
  def initialize(id, name, country)
    @id = id
    @name = name
    @country = country
  end
  def id() = @id
  def name() = @name
  def country() = @country
end

class LibraryBook
  def initialize(id, title)
    @id = id
    @title = title
  end
  def title() = @title
end

class LibraryAccount
  def initialize(id, balance, lock_version)
    @id = id
    @balance = balance
    @lock_version = lock_version
  end
  def balance() = @balance
  def lock_version() = @lock_version
end

# Exercises ActiveRecord::Model (README.md's "an optional Rails-flavored
# layer") against a real second dialect -- it adds no SQL rendering of
# its own, but #save's create path (db.last_insert_row_id()) is
# genuinely per-driver machinery worth confirming end to end here rather
# than assuming it behaves the same as it does against SQLite.
class PgAuthor < ActiveRecord::Model
  attr_accessor name: String, country: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @name = attributes["name"]
    @country = attributes["country"]
  end

  def to_attributes() = {"name": @name, "country": @country}
  def repository() = @@repository

  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
end

def build_pg_author(row) = PgAuthor.new(row)

def run_tests()
  conninfo = ENV["DIAMOND_PG_TEST_CONNINFO"]
  if conninfo == nil
    raise RuntimeError.new(
      "DIAMOND_PG_TEST_CONNINFO is not set -- run via test_postgres.sh")
  end

  def map_author(row)
    LibraryAuthor.new(row["id"], row["name"], row["country"])
  end
  def map_book(row)
    LibraryBook.new(row["id"], row["title"])
  end
  def map_account(row)
    LibraryAccount.new(row["id"], row["balance"], row["lock_version"])
  end

  def test_default_visitor_breaks_on_a_real_dialect_difference(conninfo)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS ar_pg_authors")
    db.execute("CREATE TABLE ar_pg_authors (id SERIAL PRIMARY KEY, name TEXT, country TEXT)")
    db.execute("INSERT INTO ar_pg_authors (name, country) VALUES (?, ?)", ["Ada", "UK"])
    db.execute("INSERT INTO ar_pg_authors (name, country) VALUES (?, ?)", ["Grace", "USA"])

    # Leaving the visitor unset (nil, Arel's own default Arel::SQLiteVisitor)
    # against a real PostgreSQL connection isn't rejected by
    # ActiveRecord::Repository, but it's a latent correctness gap, not a
    # supported combination -- offset-only pagination is where the two
    # dialects actually diverge (SQLite's LIMIT -1 sentinel is syntax
    # Postgres rejects outright), demonstrated here directly through Arel
    # rather than through the repository (which doesn't expose pagination).
    authors = Arel.table("ar_pg_authors")
    failed_as_expected = false
    begin
      Arel.from(authors).skip(1).to_a(db)
    rescue e: PostgreSQLError
      failed_as_expected = true
    end
    Minitest.assert_equal(true, failed_as_expected)
    db.close()
  end

  def test_repository_with_explicit_postgresql_visitor(conninfo)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS ar_pg_authors")
    db.execute("CREATE TABLE ar_pg_authors (id SERIAL PRIMARY KEY, name TEXT, country TEXT)")
    db.execute("INSERT INTO ar_pg_authors (name, country) VALUES (?, ?)", ["Ada", "UK"])
    db.execute("INSERT INTO ar_pg_authors (name, country) VALUES (?, ?)", ["Grace", "USA"])
    db.execute("INSERT INTO ar_pg_authors (name, country) VALUES (?, ?)", ["Bob", "UK"])

    repository = ActiveRecord::Repository.new(
      Arel.table("ar_pg_authors"), map_author, "id", Arel::PostgreSQLVisitor.new())

    Minitest.assert_equal("Ada", repository.find(db, 1).name())
    Minitest.assert_equal(3, repository.all(db).length())

    conditions = {}
    conditions["country"] = "UK"
    Minitest.assert_equal(2, repository.where(db, conditions).length())

    Minitest.assert_equal(1, repository.create(db, {"name": "Marie", "country": "France"}))
    Minitest.assert_equal(4, repository.all(db).length())

    Minitest.assert_equal(1, repository.update(db, 1, {"country": "England"}))
    Minitest.assert_equal("England", repository.find(db, 1).country())

    Minitest.assert_equal(1, repository.delete(db, 4))
    Minitest.assert_equal(3, repository.all(db).length())

    ActiveRecord::Transaction.run(db) do
      repository.create(db, {"name": "Committed", "country": "FR"})
    end
    Minitest.assert_equal(4, repository.all(db).length())

    db.close()
  end

  def test_has_many_through_with_explicit_postgresql_visitor(conninfo)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS ar_pg_authorships")
    db.execute("DROP TABLE IF EXISTS ar_pg_books")
    db.execute("DROP TABLE IF EXISTS ar_pg_book_authors")
    db.execute("CREATE TABLE ar_pg_book_authors (id SERIAL PRIMARY KEY, name TEXT)")
    db.execute("CREATE TABLE ar_pg_books (id SERIAL PRIMARY KEY, title TEXT)")
    db.execute("CREATE TABLE ar_pg_authorships (author_id INTEGER, book_id INTEGER)")
    db.execute("INSERT INTO ar_pg_book_authors (name) VALUES (?)", ["Ada"])
    db.execute("INSERT INTO ar_pg_books (title) VALUES (?)", ["Sketch of the Analytical Engine"])
    db.execute("INSERT INTO ar_pg_books (title) VALUES (?)", ["Notes on the Analytical Engine"])
    db.execute("INSERT INTO ar_pg_authorships (author_id, book_id) VALUES (?, ?)", [1, 1])
    db.execute("INSERT INTO ar_pg_authorships (author_id, book_id) VALUES (?, ?)", [1, 2])

    visitor = Arel::PostgreSQLVisitor.new()
    authors = ActiveRecord::Repository.new(Arel.table("ar_pg_book_authors"), map_author, "id", visitor)
    books = ActiveRecord::Repository.new(Arel.table("ar_pg_books"), map_book, "id", visitor)
    author_books = ActiveRecord::HasManyThrough.new(
      books, Arel.table("ar_pg_authorships"), "author_id", "book_id")

    ada = authors.find(db, 1)
    ada_books = author_books.all(db, ada.id())
    Minitest.assert_equal(2, ada_books.length())
    Minitest.assert_equal("Sketch of the Analytical Engine", ada_books[0].title())
    Minitest.assert_equal("Notes on the Analytical Engine", ada_books[1].title())
    db.close()
  end

  def test_optimistic_locking_with_explicit_postgresql_visitor(conninfo)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS ar_pg_accounts")
    db.execute(
      "CREATE TABLE ar_pg_accounts (id SERIAL PRIMARY KEY, balance INTEGER, lock_version INTEGER)")
    db.execute(
      "INSERT INTO ar_pg_accounts (balance, lock_version) VALUES (?, ?)", [100, 0])

    visitor = Arel::PostgreSQLVisitor.new()
    repository = ActiveRecord::Repository.new(
      Arel.table("ar_pg_accounts"), map_account, "id", visitor, nil, nil, nil, "lock_version")

    account = repository.find(db, 1)
    Minitest.assert_equal(1, repository.update(db, 1, {"balance": 150}, account.lock_version()))
    Minitest.assert_equal(1, repository.find(db, 1).lock_version())

    message = nil
    begin
      repository.update(db, 1, {"balance": 200}, account.lock_version())
    rescue error: ActiveRecord::StaleObjectError
      message = error.message()
    end
    Minitest.assert_equal("attempted to update a stale object (id=1)", message)
    Minitest.assert_equal(150, repository.find(db, 1).balance())
    db.close()
  end

  def test_preload_with_explicit_postgresql_visitor(conninfo)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS ar_pg_authorships")
    db.execute("DROP TABLE IF EXISTS ar_pg_books")
    db.execute("DROP TABLE IF EXISTS ar_pg_book_authors")
    db.execute("CREATE TABLE ar_pg_book_authors (id SERIAL PRIMARY KEY, name TEXT)")
    db.execute("CREATE TABLE ar_pg_books (id SERIAL PRIMARY KEY, title TEXT, author_id INTEGER)")
    db.execute("CREATE TABLE ar_pg_authorships (author_id INTEGER, book_id INTEGER)")
    db.execute("INSERT INTO ar_pg_book_authors (name) VALUES (?)", ["Ada"])
    db.execute("INSERT INTO ar_pg_book_authors (name) VALUES (?)", ["Grace"])
    db.execute(
      "INSERT INTO ar_pg_books (title, author_id) VALUES (?, ?)", ["Sketch", 1])
    db.execute(
      "INSERT INTO ar_pg_books (title, author_id) VALUES (?, ?)", ["Notes", 1])
    db.execute("INSERT INTO ar_pg_authorships (author_id, book_id) VALUES (?, ?)", [1, 1])
    db.execute("INSERT INTO ar_pg_authorships (author_id, book_id) VALUES (?, ?)", [1, 2])

    visitor = Arel::PostgreSQLVisitor.new()
    authors = ActiveRecord::Repository.new(Arel.table("ar_pg_book_authors"), map_author, "id", visitor)
    books = ActiveRecord::Repository.new(Arel.table("ar_pg_books"), map_book, "id", visitor)

    author_books = ActiveRecord::HasMany.new(books, "author_id")
    grouped = author_books.preload(db, [1, 2])
    Minitest.assert_equal(2, grouped[1].length())
    Minitest.assert_equal(0, grouped[2].length())

    author_books_through = ActiveRecord::HasManyThrough.new(
      books, Arel.table("ar_pg_authorships"), "author_id", "book_id")
    grouped_through = author_books_through.preload(db, [1, 2])
    Minitest.assert_equal(2, grouped_through[1].length())
    Minitest.assert_equal(0, grouped_through[2].length())
    db.close()
  end

  def test_model_with_explicit_postgresql_visitor(conninfo)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS ar_pg_model_authors")
    db.execute("CREATE TABLE ar_pg_model_authors (id SERIAL PRIMARY KEY, name TEXT, country TEXT)")

    PgAuthor.configure(ActiveRecord::Repository.new(
      Arel.table("ar_pg_model_authors"), build_pg_author, "id", Arel::PostgreSQLVisitor.new()))

    ada = PgAuthor.new({"name": "Ada", "country": "UK"})
    Minitest.assert_equal(false, ada.persisted?())
    ada.save(db)
    Minitest.assert_equal(true, ada.persisted?())
    Minitest.assert_equal(1, PgAuthor.all(db).length())

    reloaded = PgAuthor.find(db, ada.id())
    Minitest.assert_equal("Ada", reloaded.name())

    reloaded.name = "Ada Lovelace"
    reloaded.save(db)
    Minitest.assert_equal("Ada Lovelace", PgAuthor.find(db, ada.id()).name())

    ada.destroy(db)
    Minitest.assert_equal(0, PgAuthor.all(db).length())
    db.close()
  end

  def test_batches_with_explicit_postgresql_visitor(conninfo)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS ar_pg_batch_authors")
    db.execute("CREATE TABLE ar_pg_batch_authors (id SERIAL PRIMARY KEY, name TEXT, country TEXT)")
    index = 0
    while index < 25
      db.execute(
        "INSERT INTO ar_pg_batch_authors (name, country) VALUES (?, ?)", ["Author #{index}", "UK"])
      index += 1
    end

    repository = ActiveRecord::Repository.new(
      Arel.table("ar_pg_batch_authors"), map_author, "id", Arel::PostgreSQLVisitor.new())

    seen_ids = []
    def collect_id(author)
      seen_ids.push(author.id())
    end
    repository.find_each(db, collect_id, 7)
    Minitest.assert_equal(25, seen_ids.length())
    Minitest.assert_equal(1, seen_ids[0])
    Minitest.assert_equal(25, seen_ids[24])

    batch_sizes = []
    def collect_batch_size(batch)
      batch_sizes.push(batch.length())
    end
    repository.find_in_batches(db, collect_batch_size, 10)
    Minitest.assert_equal(3, batch_sizes.length())
    Minitest.assert_equal(10, batch_sizes[0])
    Minitest.assert_equal(5, batch_sizes[2])
    db.close()
  end

  def test_nested_transaction_with_explicit_postgresql_visitor(conninfo)
    db = PostgreSQL.open(conninfo)
    db.execute("DROP TABLE IF EXISTS ar_pg_nested_authors")
    db.execute("CREATE TABLE ar_pg_nested_authors (id SERIAL PRIMARY KEY, name TEXT, country TEXT)")
    repository = ActiveRecord::Repository.new(Arel.table("ar_pg_nested_authors"), map_author)

    ActiveRecord::Transaction.run(db) do
      repository.create(db, {"name": "Ada", "country": "UK"})
      failed = false
      begin
        ActiveRecord::Transaction.run_nested(db, "before_grace") do
          repository.create(db, {"name": "Grace", "country": "USA"})
          raise RuntimeError.new("oops")
        end
      rescue error: RuntimeError
        failed = true
      end
      Minitest.assert_equal(true, failed)
    end
    Minitest.assert_equal(1, repository.all(db).length())
    Minitest.assert_equal("Ada", repository.all(db)[0].name())
    db.close()
  end

  suite = Minitest.new()
  suite.test("default visitor breaks on a real dialect difference") do
    test_default_visitor_breaks_on_a_real_dialect_difference(conninfo)
  end
  suite.test("repository with explicit Arel::PostgreSQLVisitor") do
    test_repository_with_explicit_postgresql_visitor(conninfo)
  end
  suite.test("HasManyThrough with explicit Arel::PostgreSQLVisitor") do
    test_has_many_through_with_explicit_postgresql_visitor(conninfo)
  end
  suite.test("optimistic locking with explicit Arel::PostgreSQLVisitor") do
    test_optimistic_locking_with_explicit_postgresql_visitor(conninfo)
  end
  suite.test("preload with explicit Arel::PostgreSQLVisitor") do
    test_preload_with_explicit_postgresql_visitor(conninfo)
  end
  suite.test("Model with explicit Arel::PostgreSQLVisitor") do
    test_model_with_explicit_postgresql_visitor(conninfo)
  end
  suite.test("batches with explicit Arel::PostgreSQLVisitor") do
    test_batches_with_explicit_postgresql_visitor(conninfo)
  end
  suite.test("nested transaction with explicit Arel::PostgreSQLVisitor") do
    test_nested_transaction_with_explicit_postgresql_visitor(conninfo)
  end
  suite.run!()
end

run_tests()
