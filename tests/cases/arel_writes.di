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

  suite = Minitest.new()
  suite.test("INSERT", test_insert_renders_and_executes)
  suite.test("UPDATE", test_update_renders_and_executes)
  suite.run()
end

run_tests()
