require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_insert_renders_and_executes()
    items = Arel.table("odd\"items")
    payload = "pens'); DROP TABLE \"odd\"\"items\"; --"
    insert = Arel.insert_into(items).values({"na\"me": payload, "q\"ty": 3})
    sql, params = insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"odd\"\"items\" (\"na\"\"me\", \"q\"\"ty\") VALUES (?, ?)", sql)
    Minitest.assert_equal(payload, params[0])
    Minitest.assert_equal(3, params[1])

    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE \"odd\"\"items\" (\"na\"\"me\" TEXT, \"q\"\"ty\" INTEGER)")
    Minitest.assert_equal(1, insert.execute(db))
    rows = db.query("SELECT * FROM \"odd\"\"items\"")
    Minitest.assert_equal(payload, rows[0]["na\"me"])
    Minitest.assert_equal(1, rows.length())
    db.close()
  end

  def test_update_renders_and_executes()
    items = Arel.table("odd\"items")
    payload = "4; DROP TABLE \"odd\"\"items\"; --"
    update = Arel.update(items).set({"q\"ty": payload})
    update = update.where(items.column("na\"me").eq("pens"))
    sql, params = update.to_sql()
    Minitest.assert_equal("UPDATE \"odd\"\"items\" SET \"q\"\"ty\" = ? WHERE \"odd\"\"items\".\"na\"\"me\" = ?", sql)
    Minitest.assert_equal(payload, params[0])
    Minitest.assert_equal("pens", params[1])

    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE \"odd\"\"items\" (\"na\"\"me\" TEXT, \"q\"\"ty\" INTEGER)")
    db.execute("INSERT INTO \"odd\"\"items\" VALUES (?, ?)", ["pens", 3])
    Minitest.assert_equal(1, update.execute(db))
    Minitest.assert_equal(payload,
      db.query("SELECT \"q\"\"ty\" FROM \"odd\"\"items\"")[0]["q\"ty"])
    Minitest.assert_equal(1, db.query("SELECT * FROM \"odd\"\"items\"").length())
    db.close()
  end

  def test_delete_renders_and_executes()
    items = Arel.table("odd\"items")
    payload = "remove'); DROP TABLE \"odd\"\"items\"; --"
    deletion = Arel.delete_from(items).where(items.column("na\"me").eq(payload))
    sql, params = deletion.to_sql()
    Minitest.assert_equal("DELETE FROM \"odd\"\"items\" WHERE \"odd\"\"items\".\"na\"\"me\" = ?", sql)
    Minitest.assert_equal(1, params.length())
    Minitest.assert_equal(payload, params[0])

    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE \"odd\"\"items\" (\"na\"\"me\" TEXT, \"q\"\"ty\" INTEGER)")
    db.execute("INSERT INTO \"odd\"\"items\" VALUES (?, 0)", [payload])
    db.execute("INSERT INTO \"odd\"\"items\" VALUES ('keep', 2)")
    Minitest.assert_equal(1, deletion.execute(db))
    Minitest.assert_equal(2, db.query("SELECT \"q\"\"ty\" FROM \"odd\"\"items\"")[0]["q\"ty"])
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
    begin
      Arel.insert_into(items).values({"": 1}).to_sql()
    rescue error: ArgumentError
      messages.push(error.message())
    end
    begin
      Arel.update(items).set({"": 1}).all().to_sql()
    rescue error: ArgumentError
      messages.push(error.message())
    end
    begin
      Arel.insert_into(items).values({"id": 1}).on_conflict_do_nothing([""]).to_sql()
    rescue error: ArgumentError
      messages.push(error.message())
    end
    Minitest.assert_equal("INSERT requires at least one value", messages[0])
    Minitest.assert_equal("UPDATE requires where() or explicit all()", messages[1])
    Minitest.assert_equal("DELETE requires where() or explicit all()", messages[2])
    Minitest.assert_equal("SQL identifier cannot be empty", messages[3])
    Minitest.assert_equal("SQL identifier cannot be empty", messages[4])
    Minitest.assert_equal("SQL identifier cannot be empty", messages[5])
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

  def test_insert_can_ignore_conflicts()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE \"odd\"\"tags\" (\"na\"\"me\" TEXT UNIQUE)")
    tags = Arel.table("odd\"tags")
    insert = Arel.insert_into(tags).values({"na\"me": "ruby"})
    insert = insert.on_conflict_do_nothing(["na\"me"])
    sql, params = insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"odd\"\"tags\" (\"na\"\"me\") VALUES (?) ON CONFLICT (\"na\"\"me\") DO NOTHING", sql)
    Minitest.assert_equal(1, insert.execute(db))
    Minitest.assert_equal(0, insert.execute(db))
    Minitest.assert_equal(1, db.query("SELECT * FROM \"odd\"\"tags\"").length())
    db.close()
  end

  def test_insert_can_update_on_conflict()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE inventory (name TEXT UNIQUE, qty INTEGER)")
    db.execute("INSERT INTO inventory VALUES ('pens', 2)")
    inventory = Arel.table("inventory")
    payload = "5; DROP TABLE inventory; --"
    insert = Arel.insert_into(inventory).values({"name": "pens", "qty": 4})
    insert = insert.on_conflict_do_update(["name"], {
      "qty": payload
    })
    sql, params = insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"inventory\" (\"name\", \"qty\") VALUES (?, ?) ON CONFLICT (\"name\") DO UPDATE SET \"qty\" = ?", sql)
    Minitest.assert_equal(payload, params[2])
    insert.execute(db)
    Minitest.assert_equal(payload, db.query("SELECT qty FROM inventory")[0]["qty"])
    Minitest.assert_equal(1, db.query("SELECT * FROM inventory").length())
    db.close()
  end

  def test_conflict_update_requires_assignments()
    items = Arel.table("items")
    insert = Arel.insert_into(items).values({"id": 1})
    insert = insert.on_conflict_do_update(["id"], {})
    message = nil
    begin
      insert.to_sql()
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("conflict update requires at least one assignment", message)
  end

  def test_insert_select_validates_projection_count()
    items = Arel.table("items")
    query = Arel.from(items).project([items.column("name"), items.column("qty")])
    insert = Arel.insert_into(items).from_query(["name"], query)
    message = nil
    begin
      insert.to_sql()
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("INSERT SELECT columns must match query projections", message)
  end

  def test_insert_select_rejects_unknown_wildcard_shape()
    items = Arel.table("items")
    insert = Arel.insert_into(items).from_query(["name"], Arel.from(items))
    message = nil
    begin
      insert.to_sql()
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("INSERT SELECT requires explicit projections", message)
  end

  def test_multi_row_insert_returns_each_inserted_row()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT)")
    notes = Arel.table("notes")
    insert = Arel.insert_into(notes).values_many([
      {"body": "first"}, {"body": "second"}
    ]).returning([notes.column("id"), notes.column("body")])
    rows = insert.to_a(db)
    Minitest.assert_equal(2, rows.length())
    Minitest.assert_equal(1, rows[0]["id"])
    Minitest.assert_equal("first", rows[0]["body"])
    Minitest.assert_equal(2, rows[1]["id"])
    Minitest.assert_equal("second", rows[1]["body"])
    db.close()
  end

  def test_insert_select_accepts_ctes()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE source_items (name TEXT, qty INTEGER)")
    db.execute("CREATE TABLE archived_items (name TEXT)")
    db.execute("INSERT INTO source_items VALUES ('pens', 3), ('pencils', 2), ('paper', 1)")
    source = Arel.table("source_items")
    eligible_query = Arel.from(source).project(source.column("name"))
    eligible_query = eligible_query.where(source.column("qty").gt(1))
    eligible = Arel.table("eligible")
    selection = Arel.from(eligible).project(eligible.column("name"))
    selection = selection.order(eligible.column("name").asc()).take(1)
    target = Arel.table("archived_items")
    insert = Arel.insert_into(target).with("eligible", eligible_query)
    insert = insert.from_query(["name"], selection)
    sql, params = insert.to_sql()
    Minitest.assert_equal("WITH \"eligible\" AS (SELECT \"source_items\".\"name\" FROM \"source_items\" WHERE \"source_items\".\"qty\" > ?) INSERT INTO \"archived_items\" (\"name\") SELECT \"eligible\".\"name\" FROM \"eligible\" ORDER BY \"eligible\".\"name\" ASC LIMIT ?", sql)
    Minitest.assert_equal(1, params[0])
    Minitest.assert_equal(1, params[1])
    insert.execute(db)
    Minitest.assert_equal("pencils", db.query("SELECT name FROM archived_items")[0]["name"])
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
  suite.test("INSERT conflict ignore", test_insert_can_ignore_conflicts)
  suite.test("INSERT conflict update", test_insert_can_update_on_conflict)
  suite.test("conflict update validation", test_conflict_update_requires_assignments)
  suite.test("INSERT SELECT validation", test_insert_select_validates_projection_count)
  suite.test("INSERT SELECT wildcard validation", test_insert_select_rejects_unknown_wildcard_shape)
  suite.test("multi-row INSERT RETURNING", test_multi_row_insert_returns_each_inserted_row)
  suite.test("INSERT CTE", test_insert_select_accepts_ctes)
  suite.run!()
end

run_tests()
