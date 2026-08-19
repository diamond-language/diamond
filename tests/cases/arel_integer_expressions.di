require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_modulo_is_structural()
    values = Arel.table("values_table")
    expression = values.column("value").modulo(2)
    query = Arel.from(values).project(Arel.as(expression, "remainder"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT (\"values_table\".\"value\" % ?) AS \"remainder\" FROM \"values_table\"", sql)
    Minitest.assert_equal(2, params[0])
  end

  suite = Minitest.new()
  suite.test("structural modulo", test_modulo_is_structural)
  suite.run()
end

run_tests()
