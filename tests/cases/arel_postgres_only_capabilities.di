# Two capabilities added alongside a portable Cast fix (parameterized cast
# types, e.g. NUMERIC(10,2), needed no visitor capability at all -- verified
# directly that both dialects already accept it identically, see
# arel_expressions.di) are genuinely PostgreSQL-only, verified directly
# against a real SQLite error before being modeled this way: per-column
# DEFAULT in a VALUES row, and named-constraint ON CONFLICT targets.
# SQLiteVisitor (the default, when no visitor is passed) correctly rejects
# both; real execution against a live PostgreSQL server is covered by
# packages/arel/test_postgres_dialect.di instead, since this suite stays
# self-contained like every other tests/cases fixture.
require "../../lib/minitest"
require "../../packages/arel/lib/arel"

def run_tests()
  def test_column_default_is_a_postgres_only_capability()
    items = Arel.table("items")
    insert = Arel.insert_into(items).values({"name": "pens", "qty": Arel.column_default()})
    message = nil
    begin
      insert.to_sql()
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("SQLite visitor does not support per-column default values", message)
  end

  def test_named_constraint_target_is_postgres_only()
    items = Arel.table("items")
    insert = Arel.insert_into(items).values({"name": "pens"})
    insert = insert.on_conflict_do_nothing(Arel.conflict_target_on_constraint("items_name_key"))
    message = nil
    begin
      insert.to_sql()
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal(
      "SQLite visitor does not support named-constraint conflict targets", message)
  end

  def test_named_constraint_conflict_target_renders_correctly()
    items = Arel.table("items")
    insert = Arel.insert_into(items).values({"name": "pens", "qty": 4})
    insert = insert.on_conflict_do_update(
      Arel.conflict_target_on_constraint("items_name_key"),
      {"qty": Arel.expression(Arel.excluded("qty"))})
    sql, params = insert.to_sql(Arel::PostgreSQLVisitor.new())
    Minitest.assert_equal(
      "INSERT INTO \"items\" (\"name\", \"qty\") VALUES (?, ?) " +
      "ON CONFLICT ON CONSTRAINT \"items_name_key\" DO UPDATE SET \"qty\" = excluded.\"qty\"",
      sql)
    Minitest.assert_equal("pens", params[0])
    Minitest.assert_equal(4, params[1])
  end

  def test_column_default_renders_correctly()
    items = Arel.table("items")
    insert = Arel.insert_into(items).values({"name": "pens", "qty": Arel.column_default()})
    sql, params = insert.to_sql(Arel::PostgreSQLVisitor.new())
    Minitest.assert_equal("INSERT INTO \"items\" (\"name\", \"qty\") VALUES (?, DEFAULT)", sql)
    Minitest.assert_equal(1, params.length())
    Minitest.assert_equal("pens", params[0])
  end

  suite = Minitest.new()
  suite.test("per-column DEFAULT is Postgres-only",
             test_column_default_is_a_postgres_only_capability)
  suite.test("named-constraint conflict target is Postgres-only",
             test_named_constraint_target_is_postgres_only)
  suite.test("named-constraint conflict target renders correctly",
             test_named_constraint_conflict_target_renders_correctly)
  suite.test("per-column DEFAULT renders correctly", test_column_default_renders_correctly)
  suite.run!()
end

run_tests()
