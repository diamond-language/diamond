require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_union_combines_queries_and_binds()
    people = Arel.table("people")
    adults = Arel.from(people).project(people.column("name"))
    adults = adults.where(people.column("age").gteq(18))
    minors = Arel.from(people).project(people.column("name"))
    minors = minors.where(people.column("age").lt(18))
    sql, params = Arel.union(adults, minors).to_sql()
    Minitest.assert_equal("SELECT \"people\".\"name\" FROM \"people\" WHERE \"people\".\"age\" >= ? UNION SELECT \"people\".\"name\" FROM \"people\" WHERE \"people\".\"age\" < ?", sql)
    Minitest.assert_equal(18, params[0])
    Minitest.assert_equal(18, params[1])
  end

  def test_union_all_preserves_duplicates_in_sqlite()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE values_table (value INTEGER)")
    db.execute("INSERT INTO values_table VALUES (?)", [1])
    values = Arel.table("values_table")
    branch = Arel.from(values).project(values.column("value"))
    rows = Arel.union_all(branch, branch).to_a(db)
    Minitest.assert_equal(2, rows.length())
    Minitest.assert_equal(1, rows[0]["value"])
    Minitest.assert_equal(1, rows[1]["value"])
    db.close()
  end

  suite = Minitest.new()
  suite.test("UNION", test_union_combines_queries_and_binds)
  suite.test("UNION ALL", test_union_all_preserves_duplicates_in_sqlite)
  suite.run()
end

run_tests()
