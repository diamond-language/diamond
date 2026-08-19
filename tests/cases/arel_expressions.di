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

  def test_function_results_support_pattern_predicates()
    people = Arel.table("people")
    lowered = Arel.lower(people.column("name"))
    query = Arel.from(people).where(lowered.like("a%").and_also(lowered.not_like("admin%")))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE (LOWER(\"people\".\"name\") LIKE ? AND LOWER(\"people\".\"name\") NOT LIKE ?)", sql)
    Minitest.assert_equal(2, params.length())
    Minitest.assert_equal("a%", params[0])
    Minitest.assert_equal("admin%", params[1])
  end

  def test_function_results_support_membership_predicates()
    people = Arel.table("people")
    lowered = Arel.lower(people.column("role"))
    query = Arel.from(people).where(lowered.in_list(["admin", "editor"]))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE LOWER(\"people\".\"role\") IN (?, ?)", sql)
    Minitest.assert_equal("admin", params[0])
    Minitest.assert_equal("editor", params[1])
  end

  def test_function_results_support_range_predicates()
    people = Arel.table("people")
    length = Arel.function("LENGTH", [people.column("name")])
    query = Arel.from(people).where(length.between(3, 8))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE LENGTH(\"people\".\"name\") BETWEEN ? AND ?", sql)
    Minitest.assert_equal(3, params[0])
    Minitest.assert_equal(8, params[1])
  end

  def test_function_results_support_ordering()
    people = Arel.table("people")
    lowered = Arel.lower(people.column("name"))
    query = Arel.from(people).project(people.column("name")).order(lowered.asc().nulls_last())
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT \"people\".\"name\" FROM \"people\" ORDER BY LOWER(\"people\".\"name\") ASC NULLS LAST", sql)
  end

  def test_function_results_support_aliases()
    people = Arel.table("people")
    query = Arel.from(people).project(Arel.lower(people.column("name")).as("normalized_name"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT LOWER(\"people\".\"name\") AS \"normalized_name\" FROM \"people\"", sql)
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
  suite.test("function pattern predicates", test_function_results_support_pattern_predicates)
  suite.test("function membership predicates", test_function_results_support_membership_predicates)
  suite.test("function range predicates", test_function_results_support_range_predicates)
  suite.test("function ordering", test_function_results_support_ordering)
  suite.test("function aliases", test_function_results_support_aliases)
  suite.test("structural CAST", test_casts_are_structural_and_preserve_binds)
  suite.test("CAST type validation", test_cast_types_reject_sql_fragments)
  suite.test("structural concatenation", test_string_concatenation_is_structural)
  suite.run!()
end

run_tests()
