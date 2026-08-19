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
    expression = Arel.integer_operator(permissions.column("mask"), "&", 4)
    query = Arel.from(permissions).project(Arel.as(expression, "selected"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT (\"permissions\".\"mask\" & ?) AS \"selected\" FROM \"permissions\"", sql)
    Minitest.assert_equal(4, params[0])
  end

  def test_bitwise_or_is_structural()
    permissions = Arel.table("permissions")
    expression = Arel.integer_operator(permissions.column("mask"), "|", 2)
    query = Arel.from(permissions).project(Arel.as(expression, "combined"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT (\"permissions\".\"mask\" | ?) AS \"combined\" FROM \"permissions\"", sql)
    Minitest.assert_equal(2, params[0])
  end

  def test_left_shift_is_structural()
    flags = Arel.table("flags")
    expression = Arel.integer_operator(flags.column("mask"), "<<", 2)
    query = Arel.from(flags).project(Arel.as(expression, "shifted"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT (\"flags\".\"mask\" << ?) AS \"shifted\" FROM \"flags\"", sql)
    Minitest.assert_equal(2, params[0])
  end

  def test_right_shift_is_structural()
    flags = Arel.table("flags")
    expression = Arel.integer_operator(flags.column("mask"), ">>", 1)
    query = Arel.from(flags).project(Arel.as(expression, "shifted"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT (\"flags\".\"mask\" >> ?) AS \"shifted\" FROM \"flags\"", sql)
    Minitest.assert_equal(1, params[0])
  end

  suite = Minitest.new()
  suite.test("structural modulo", test_modulo_is_structural)
  suite.test("structural bitwise AND", test_bitwise_and_is_structural)
  suite.test("structural bitwise OR", test_bitwise_or_is_structural)
  suite.test("structural left shift", test_left_shift_is_structural)
  suite.test("structural right shift", test_right_shift_is_structural)
  suite.run()
end

run_tests()
