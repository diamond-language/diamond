require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_insert_renders_and_executes()
    items = Arel.table("items")
    insert = Arel.insert_into(items).values({"name": "pens", "qty": 3})
    sql, params = insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"items\" (\"name\", \"qty\") VALUES (?, ?)", sql)
    Minitest.assert_equal("pens", params[0])
    Minitest.assert_equal(3, params[1])

    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE items (name TEXT, qty INTEGER)")
    Minitest.assert_equal(1, insert.execute(db))
    rows = db.query("SELECT * FROM items")
    Minitest.assert_equal("pens", rows[0]["name"])
    db.close()
  end

  def test_update_renders_and_executes()
    items = Arel.table("items")
    update = Arel.update(items).set({"qty": 4})
    update = update.where(items.column("name").eq("pens"))
    sql, params = update.to_sql()
    Minitest.assert_equal("UPDATE \"items\" SET \"qty\" = ? WHERE \"items\".\"name\" = ?", sql)
    Minitest.assert_equal(4, params[0])
    Minitest.assert_equal("pens", params[1])

    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE items (name TEXT, qty INTEGER)")
    db.execute("INSERT INTO items VALUES (?, ?)", ["pens", 3])
    Minitest.assert_equal(1, update.execute(db))
    Minitest.assert_equal(4, db.query("SELECT qty FROM items")[0]["qty"])
    db.close()
  end

  def test_delete_renders_and_executes()
    items = Arel.table("items")
    deletion = Arel.delete_from(items).where(items.column("qty").lt(1))
    sql, params = deletion.to_sql()
    Minitest.assert_equal("DELETE FROM \"items\" WHERE \"items\".\"qty\" < ?", sql)
    Minitest.assert_equal(1, params.length())
    Minitest.assert_equal(1, params[0])

    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE items (qty INTEGER)")
    db.execute("INSERT INTO items VALUES (0)")
    db.execute("INSERT INTO items VALUES (2)")
    Minitest.assert_equal(1, deletion.execute(db))
    Minitest.assert_equal(2, db.query("SELECT qty FROM items")[0]["qty"])
    db.close()
  end

  def test_returning_is_structural_and_returns_rows()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)")
    items = Arel.table("items")
    insert = Arel.insert_into(items).values({"name": "paper"})
    rows = insert.returning([items.column("id"), items.column("name")]).to_a(db)
    Minitest.assert_equal(1, rows.length())
    Minitest.assert_equal(1, rows[0]["id"])
    Minitest.assert_equal("paper", rows[0]["name"])

    update = Arel.update(items).set({"name": "card"})
    updated = update.all().returning(items.column("name")).to_a(db)
    Minitest.assert_equal("card", updated[0]["name"])

    deleted = Arel.delete_from(items).all().returning(items.column("id")).to_a(db)
    Minitest.assert_equal(1, deleted[0]["id"])
    db.close()
  end

  def test_write_validation_requires_values_and_explicit_scope()
    items = Arel.table("items")
    messages = []
    begin
      Arel.insert_into(items).values({}).to_sql()
    rescue error: ArgumentError
      messages.push(error.message())
    end
    begin
      Arel.update(items).set({"qty": 1}).to_sql()
    rescue error: ArgumentError
      messages.push(error.message())
    end
    begin
      Arel.delete_from(items).to_sql()
    rescue error: ArgumentError
      messages.push(error.message())
    end
    Minitest.assert_equal("INSERT requires at least one value", messages[0])
    Minitest.assert_equal("UPDATE requires where() or explicit all()", messages[1])
    Minitest.assert_equal("DELETE requires where() or explicit all()", messages[2])
  end

  def test_multi_row_insert_preserves_row_and_bind_order()
    items = Arel.table("items")
    insert = Arel.insert_into(items).values_many([
      {"name": "pens", "qty": 3},
      {"name": "paper", "qty": 5}
    ])
    sql, params = insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"items\" (\"name\", \"qty\") VALUES (?, ?), (?, ?)", sql)
    Minitest.assert_equal("pens", params[0])
    Minitest.assert_equal(3, params[1])
    Minitest.assert_equal("paper", params[2])
    Minitest.assert_equal(5, params[3])
  end

  def test_multi_row_insert_requires_identical_columns()
    items = Arel.table("items")
    insert = Arel.insert_into(items).values_many([
      {"name": "pens", "qty": 3},
      {"name": "paper", "price": 5}
    ])
    message = nil
    begin
      insert.to_sql()
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("INSERT rows must have identical columns", message)
  end

  def test_insert_select_preserves_query_binds_and_executes()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE source_items (name TEXT, qty INTEGER)")
    db.execute("CREATE TABLE archived_items (name TEXT, qty INTEGER)")
    db.execute("INSERT INTO source_items VALUES ('pens', 3), ('paper', 1)")
    source = Arel.table("source_items")
    query = Arel.from(source).project([
      source.column("name"), source.column("qty")
    ]).where(source.column("qty").gt(1))
    target = Arel.table("archived_items")
    insert = Arel.insert_into(target).from_query(["name", "qty"], query)
    sql, params = insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"archived_items\" (\"name\", \"qty\") SELECT \"source_items\".\"name\", \"source_items\".\"qty\" FROM \"source_items\" WHERE \"source_items\".\"qty\" > ?", sql)
    Minitest.assert_equal(1, params[0])
    Minitest.assert_equal(1, insert.execute(db))
    Minitest.assert_equal("pens", db.query("SELECT name FROM archived_items")[0]["name"])
    db.close()
  end

  def test_update_assignments_accept_expressions()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE counters (id INTEGER, value INTEGER)")
    db.execute("INSERT INTO counters VALUES (1, 4)")
    counters = Arel.table("counters")
    update = Arel.update(counters).set({
      "value": Arel.expression(Arel.sql("\"value\" + ?", [3]))
    }).where(counters.column("id").eq(1))
    sql, params = update.to_sql()
    Minitest.assert_equal("UPDATE \"counters\" SET \"value\" = \"value\" + ? WHERE \"counters\".\"id\" = ?", sql)
    Minitest.assert_equal(3, params[0])
    Minitest.assert_equal(1, params[1])
    update.execute(db)
    Minitest.assert_equal(7, db.query("SELECT value FROM counters")[0]["value"])
    db.close()
  end

  suite = Minitest.new()
  suite.test("INSERT", test_insert_renders_and_executes)
  suite.test("UPDATE", test_update_renders_and_executes)
  suite.test("DELETE", test_delete_renders_and_executes)
  suite.test("RETURNING", test_returning_is_structural_and_returns_rows)
  suite.test("write validation", test_write_validation_requires_values_and_explicit_scope)
  suite.test("multi-row INSERT", test_multi_row_insert_preserves_row_and_bind_order)
  suite.test("multi-row INSERT shape", test_multi_row_insert_requires_identical_columns)
  suite.test("INSERT SELECT", test_insert_select_preserves_query_binds_and_executes)
  suite.test("UPDATE expressions", test_update_assignments_accept_expressions)
  suite.run()
end

run_tests()
