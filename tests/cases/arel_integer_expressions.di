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

  def test_bitwise_and_is_structural()
    permissions = Arel.table("permissions")
    expression = Arel.bit_and(permissions.column("mask"), 4)
    query = Arel.from(permissions).project(Arel.as(expression, "selected"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT (\"permissions\".\"mask\" & ?) AS \"selected\" FROM \"permissions\"", sql)
    Minitest.assert_equal(4, params[0])
  end

  def test_bitwise_or_is_structural()
    permissions = Arel.table("permissions")
    expression = Arel.bit_or(permissions.column("mask"), 2)
    query = Arel.from(permissions).project(Arel.as(expression, "combined"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT (\"permissions\".\"mask\" | ?) AS \"combined\" FROM \"permissions\"", sql)
    Minitest.assert_equal(2, params[0])
  end

  suite = Minitest.new()
  suite.test("structural modulo", test_modulo_is_structural)
  suite.test("structural bitwise AND", test_bitwise_and_is_structural)
  suite.test("structural bitwise OR", test_bitwise_or_is_structural)
  suite.run()
end

run_tests()
