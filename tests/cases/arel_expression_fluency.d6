require "../../lib/minitest"
require "../../packages/arel/lib/arel"

def run_tests()
  def test_function_results_support_aliases()
    people = Arel.table("people")
    query = Arel.from(people).project(Arel.lower(people.column("name")).as("normalized_name"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT LOWER(\"people\".\"name\") AS \"normalized_name\" FROM \"people\"", sql)
  end

  def test_function_results_support_collation()
    people = Arel.table("people")
    normalized = Arel.lower(people.column("name")).collate("NOCASE")
    query = Arel.from(people).project(people.column("name")).order(Arel.asc(normalized))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT \"people\".\"name\" FROM \"people\" ORDER BY LOWER(\"people\".\"name\") COLLATE \"NOCASE\" ASC", sql)
  end

  def test_function_results_support_pattern_predicates()
    people = Arel.table("people")
    lowered = Arel.lower(people.column("name"))
    query = Arel.from(people).where(lowered.like("a%").and_also(lowered.not_like("admin%")))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE (LOWER(\"people\".\"name\") LIKE ? AND LOWER(\"people\".\"name\") NOT LIKE ?)", sql)
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

  def test_binds_on_the_left_come_before_membership_values()
    people = Arel.table("people")
    lowered = Arel.lower(people.column("name").concat("x"))
    query = Arel.from(people).where(lowered.in_list(["a", "b"]))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE LOWER((\"people\".\"name\" || ?)) IN (?, ?)", sql)
    Minitest.assert_equal(["x", "a", "b"], params)
  end

  def test_binds_on_the_left_come_before_range_bounds()
    people = Arel.table("people")
    padded = Arel.function("LENGTH", [people.column("name").concat("!")])
    query = Arel.from(people).where(padded.between(3, 8))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE LENGTH((\"people\".\"name\" || ?)) BETWEEN ? AND ?", sql)
    Minitest.assert_equal(["!", 3, 8], params)
  end

  def test_function_results_support_ordering()
    people = Arel.table("people")
    lowered = Arel.lower(people.column("name"))
    query = Arel.from(people).project(people.column("name")).order(lowered.asc().nulls_last())
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT \"people\".\"name\" FROM \"people\" ORDER BY LOWER(\"people\".\"name\") ASC NULLS LAST", sql)
  end

  suite = Minitest.new()
  suite.test("function aliases", test_function_results_support_aliases)
  suite.test("function collation", test_function_results_support_collation)
  suite.test("function pattern predicates", test_function_results_support_pattern_predicates)
  suite.test("function membership predicates", test_function_results_support_membership_predicates)
  suite.test("function range predicates", test_function_results_support_range_predicates)
  suite.test("left binds before membership values", test_binds_on_the_left_come_before_membership_values)
  suite.test("left binds before range bounds", test_binds_on_the_left_come_before_range_bounds)
  suite.test("function ordering", test_function_results_support_ordering)
  suite.run!()
end

run_tests()
