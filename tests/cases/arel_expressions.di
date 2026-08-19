require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_generic_functions_are_structural()
    people = Arel.table("people")
    fallback = Arel.sql("?", ["unknown"])
    expression = Arel.function("COALESCE", [people.column("name"), fallback])
    query = Arel.from(people).project(Arel.as(expression, "display_name"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT COALESCE(\"people\".\"name\", ?) AS \"display_name\" FROM \"people\"", sql)
    Minitest.assert_equal("unknown", params[0])
  end

  def test_generic_function_names_reject_sql_fragments()
    message = nil
    begin
      Arel.function("COUNT); DROP TABLE people; --", [])
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("SQL function name must be an identifier", message)
  end

  def test_casts_are_structural_and_preserve_binds()
    values = Arel.table("values_table")
    expression = Arel.cast(Arel.sql("?", ["42"]), "INTEGER")
    query = Arel.from(values).project(Arel.as(expression, "number"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT CAST(? AS INTEGER) AS \"number\" FROM \"values_table\"", sql)
    Minitest.assert_equal("42", params[0])
  end

  suite = Minitest.new()
  suite.test("generic SQL function", test_generic_functions_are_structural)
  suite.test("generic function validation", test_generic_function_names_reject_sql_fragments)
  suite.test("structural CAST", test_casts_are_structural_and_preserve_binds)
  suite.run()
end

run_tests()
