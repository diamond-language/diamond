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

  suite = Minitest.new()
  suite.test("UNION", test_union_combines_queries_and_binds)
  suite.run()
end

run_tests()
