require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_integer_expressions_execute_against_sqlite()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE values_table (value INTEGER)")
    db.execute("INSERT INTO values_table VALUES (13)")
    values = Arel.table("values_table")
    query = Arel.from(values).project([
      Arel.as(values.column("value").modulo(5), "modulo"),
      Arel.as(Arel.integer_operator(values.column("value"), "&", 6), "and_value"),
      Arel.as(Arel.integer_operator(values.column("value"), "|", 2), "or_value"),
      Arel.as(Arel.integer_operator(values.column("value"), "<<", 1), "left_value"),
      Arel.as(Arel.integer_operator(values.column("value"), ">>", 2), "right_value")
    ])
    row = query.to_a(db)[0]
    Minitest.assert_equal(3, row["modulo"])
    Minitest.assert_equal(4, row["and_value"])
    Minitest.assert_equal(15, row["or_value"])
    Minitest.assert_equal(26, row["left_value"])
    Minitest.assert_equal(3, row["right_value"])
    db.close()
  end

  suite = Minitest.new()
  suite.test("SQLite integer expressions", test_integer_expressions_execute_against_sqlite)
  suite.run()
end

run_tests()
