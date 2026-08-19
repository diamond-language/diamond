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

  def test_multi_row_expressions_preserve_bind_order()
    metrics = Arel.table("metrics")
    insert = Arel.insert_into(metrics).values_many([
      {"name": "first", "value": Arel.expression(Arel.sql("? + ?", [1, 2]))},
      {"name": "second", "value": Arel.expression(Arel.sql("? * ?", [3, 4]))}
    ])
    sql, params = insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"metrics\" (\"name\", \"value\") VALUES (?, ? + ?), (?, ? * ?)", sql)
    Minitest.assert_equal("first", params[0])
    Minitest.assert_equal(1, params[1])
    Minitest.assert_equal(2, params[2])
    Minitest.assert_equal("second", params[3])
    Minitest.assert_equal(3, params[4])
    Minitest.assert_equal(4, params[5])
  end

  def test_conflict_updates_reference_excluded_values_structurally()
    inventory = Arel.table("inventory")
    insert = Arel.insert_into(inventory).values({"name": "pens", "qty": 4})
    insert = insert.on_conflict_do_update(["name"], {
      "qty": Arel.expression(Arel.excluded("qty"))
    })
    sql, params = insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"inventory\" (\"name\", \"qty\") VALUES (?, ?) ON CONFLICT (\"name\") DO UPDATE SET \"qty\" = excluded.\"qty\"", sql)
    Minitest.assert_equal("pens", params[0])
    Minitest.assert_equal(4, params[1])
  end

  def test_default_values_can_return_generated_columns()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE jobs (id INTEGER PRIMARY KEY, state TEXT DEFAULT 'queued')")
    jobs = Arel.table("jobs")
    insert = Arel.insert_into(jobs).default_values()
    insert = insert.returning([jobs.column("id"), jobs.column("state")])
    sql, params = insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"jobs\" DEFAULT VALUES RETURNING \"jobs\".\"id\", \"jobs\".\"state\"", sql)
    Minitest.assert_equal(0, params.length())
    rows = insert.to_a(db)
    Minitest.assert_equal(1, rows[0]["id"])
    Minitest.assert_equal("queued", rows[0]["state"])
    db.close()
  end

  def test_conflict_targets_accept_partial_index_predicates()
    users = Arel.table("users")
    target = Arel.conflict_target(["email"])
    target = target.where(Arel.sql("\"active\" = 1"))
    insert = Arel.insert_into(users).values({"email": "a@example.test", "active": 1})
    insert = insert.on_conflict_do_nothing(target)
    sql, params = insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"users\" (\"email\", \"active\") VALUES (?, ?) ON CONFLICT (\"email\") WHERE \"active\" = 1 DO NOTHING", sql)
    Minitest.assert_equal("a@example.test", params[0])
    Minitest.assert_equal(1, params[1])
  end

  def test_partial_index_conflict_target_executes()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE users (email TEXT, active INTEGER)")
    db.execute("CREATE UNIQUE INDEX active_email ON users(email) WHERE active = 1")
    users = Arel.table("users")
    target = Arel.conflict_target(["email"])
    target = target.where(Arel.sql("\"active\" = 1"))
    insert = Arel.insert_into(users).values({"email": "a@example.test", "active": 1})
    insert = insert.on_conflict_do_nothing(target)
    Minitest.assert_equal(1, insert.execute(db))
    Minitest.assert_equal(0, insert.execute(db))
    Minitest.assert_equal(1, db.query("SELECT email FROM users").length())
    db.close()
  end

  suite = Minitest.new()
  suite.test("INSERT expressions", test_insert_values_accept_explicit_expressions)
  suite.test("multi-row INSERT expressions", test_multi_row_expressions_preserve_bind_order)
  suite.test("excluded conflict value", test_conflict_updates_reference_excluded_values_structurally)
  suite.test("INSERT DEFAULT VALUES", test_default_values_can_return_generated_columns)
  suite.test("partial conflict target", test_conflict_targets_accept_partial_index_predicates)
  suite.test("partial conflict execution", test_partial_index_conflict_target_executes)
  suite.run()
end

run_tests()
