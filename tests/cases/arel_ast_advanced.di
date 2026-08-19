require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_between_predicates_bind_both_bounds()
    people = Arel.table("people")
    range = people.column("age").between(18, 65)
    predicate = range.and_also(people.column("score").not_between(0, 10))
    sql, params = Arel.from(people).where(predicate).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE (\"people\".\"age\" BETWEEN ? AND ? AND \"people\".\"score\" NOT BETWEEN ? AND ?)", sql)
    Minitest.assert_equal(4, params.length())
    Minitest.assert_equal(18, params[0])
    Minitest.assert_equal(10, params[3])
  end

  suite = Minitest.new()
  suite.test("BETWEEN predicates", test_between_predicates_bind_both_bounds)
  suite.run()
end

run_tests()
