require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_update_accepts_a_cte()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE items (id INTEGER, qty INTEGER)")
    db.execute("INSERT INTO items VALUES (1, 1), (2, 4)")
    items = Arel.table("items")
    source = Arel.from(items).project(items.column("id"))
    source = source.where(items.column("qty").lt(3))
    selected = Arel.table("selected")
    ids = Arel.from(selected).project(selected.column("id"))
    update = Arel.update(items).with("selected", source).set({"qty": 9})
    update = update.where(items.column("id").in_subquery(ids))
    sql, params = update.to_sql()
    Minitest.assert_equal("WITH \"selected\" AS (SELECT \"items\".\"id\" FROM \"items\" WHERE \"items\".\"qty\" < ?) UPDATE \"items\" SET \"qty\" = ? WHERE \"items\".\"id\" IN (SELECT \"selected\".\"id\" FROM \"selected\")", sql)
    Minitest.assert_equal(3, params[0])
    Minitest.assert_equal(9, params[1])
    Minitest.assert_equal(1, update.execute(db))
    Minitest.assert_equal(9, db.query("SELECT qty FROM items WHERE id = 1")[0]["qty"])
    db.close()
  end

  suite = Minitest.new()
  suite.test("UPDATE CTE", test_update_accepts_a_cte)
  suite.run()
end

run_tests()
