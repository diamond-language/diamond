require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_single_cte_renders_and_binds_before_main_query()
    people = Arel.table("people")
    active = Arel.from(people).where(people.column("active").eq(true))
    active_people = Arel.table("active_people")
    query = Arel.from(active_people).with("active_people", active)
    query = query.where(active_people.column("age").gteq(18))
    sql, params = query.to_sql()
    Minitest.assert_equal("WITH \"active_people\" AS (SELECT * FROM \"people\" WHERE \"people\".\"active\" = ?) SELECT * FROM \"active_people\" WHERE \"active_people\".\"age\" >= ?", sql)
    Minitest.assert_equal(true, params[0])
    Minitest.assert_equal(18, params[1])
  end

  suite = Minitest.new()
  suite.test("single CTE", test_single_cte_renders_and_binds_before_main_query)
  suite.run()
end

run_tests()
