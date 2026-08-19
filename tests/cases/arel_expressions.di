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

  def test_cast_types_reject_sql_fragments()
    message = nil
    begin
      Arel.cast(Arel.literal(1), "INTEGER); DROP TABLE values_table; --")
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("SQL cast type must be an identifier", message)
  end

  def test_string_concatenation_is_structural()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE people (name TEXT)")
    db.execute("INSERT INTO people VALUES ('Ada')")
    people = Arel.table("people")
    expression = people.column("name").concat(" Lovelace")
    query = Arel.from(people).project(Arel.as(expression, "full_name"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT (\"people\".\"name\" || ?) AS \"full_name\" FROM \"people\"", sql)
    Minitest.assert_equal(" Lovelace", params[0])
    Minitest.assert_equal("Ada Lovelace", query.to_a(db)[0]["full_name"])
    db.close()
  end

  suite = Minitest.new()
  suite.test("generic SQL function", test_generic_functions_are_structural)
  suite.test("generic function validation", test_generic_function_names_reject_sql_fragments)
  suite.test("structural CAST", test_casts_are_structural_and_preserve_binds)
  suite.test("CAST type validation", test_cast_types_reject_sql_fragments)
  suite.test("structural concatenation", test_string_concatenation_is_structural)
  suite.run!()
end

run_tests()
