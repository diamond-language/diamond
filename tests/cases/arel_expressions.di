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

  suite = Minitest.new()
  suite.test("generic SQL function", test_generic_functions_are_structural)
  suite.run()
end

run_tests()
