require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_computed_values_support_comparison_predicates()
    items = Arel.table("items")
    total = items.column("price").multiply(2)
    query = Arel.from(items).where(total.gteq(100))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"items\" WHERE (\"items\".\"price\" * ?) >= ?", sql)
    Minitest.assert_equal(2, params[0])
    Minitest.assert_equal(100, params[1])
  end

  suite = Minitest.new()
  suite.test("computed comparison predicates", test_computed_values_support_comparison_predicates)
  suite.run!()
end

run_tests()
