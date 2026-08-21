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
  def name() = @name
  def country() = @country
end

def run_tests()
  conninfo = ENV["DIAMOND_PG_TEST_CONNINFO"]
  if conninfo == nil
    raise RuntimeError.new(
      "DIAMOND_PG_TEST_CONNINFO is not set -- run via test_postgres.sh")
  end

  def map_author(row)
    LibraryAuthor.new(row["id"], row["name"], row["country"])
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

  suite = Minitest.new()
  suite.test("default visitor breaks on a real dialect difference") do
    test_default_visitor_breaks_on_a_real_dialect_difference(conninfo)
  end
  suite.test("repository with explicit Arel::PostgreSQLVisitor") do
    test_repository_with_explicit_postgresql_visitor(conninfo)
  end
  suite.run!()
end

run_tests()
