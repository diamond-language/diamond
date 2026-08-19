require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_insert_values_accept_explicit_expressions()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE events (name TEXT, created_at TEXT)")
    events = Arel.table("events")
    insert = Arel.insert_into(events).values({
      "name": "launch",
      "created_at": Arel.expression(Arel.sql("datetime(?)", ["2026-01-02 03:04:05"]))
    })
    sql, params = insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"events\" (\"name\", \"created_at\") VALUES (?, datetime(?))", sql)
    Minitest.assert_equal("launch", params[0])
    Minitest.assert_equal("2026-01-02 03:04:05", params[1])
    insert.execute(db)
    Minitest.assert_equal("2026-01-02 03:04:05", db.query("SELECT created_at FROM events")[0]["created_at"])
    db.close()
  end

  suite = Minitest.new()
  suite.test("INSERT expressions", test_insert_values_accept_explicit_expressions)
  suite.run()
end

run_tests()
