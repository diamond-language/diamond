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

  suite = Minitest.new()
  suite.test("INSERT", test_insert_renders_and_executes)
  suite.run()
end

run_tests()
