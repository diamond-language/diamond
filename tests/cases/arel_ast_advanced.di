require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_between_predicates_bind_both_bounds()
    people = Arel.table("people")
    range = people.column("age").between(18, 65)
    predicate = range.and_also(people.column("score").not_between(0, 10))
    sql, params = Arel.from(people).where(predicate).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE (\"people\".\"age\" BETWEEN ? AND ? AND \"people\".\"score\" NOT BETWEEN ? AND ?)", sql)
    Minitest.assert_equal(4, params.length())
    Minitest.assert_equal(18, params[0])
    Minitest.assert_equal(10, params[3])
  end

  def test_like_predicates_are_bound()
    people = Arel.table("people")
    starts_with_a = people.column("name").like("A%")
    predicate = starts_with_a.and_also(people.column("email").not_like("%@spam.test"))
    sql, params = Arel.from(people).where(predicate).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE (\"people\".\"name\" LIKE ? AND \"people\".\"email\" NOT LIKE ?)", sql)
    Minitest.assert_equal("A%", params[0])
    Minitest.assert_equal("%@spam.test", params[1])
  end

  def test_function_and_aggregate_projections()
    people = Arel.table("people")
    query = Arel.from(people).project([
      Arel.count(people.column("id")),
      Arel.avg(people.column("age")),
      Arel.upper(people.column("name"))
    ])
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT COUNT(\"people\".\"id\"), AVG(\"people\".\"age\"), UPPER(\"people\".\"name\") FROM \"people\"", sql)
    Minitest.assert_empty(params)
  end

  def test_group_by_accepts_expression_arrays()
    people = Arel.table("people")
    query = Arel.from(people).project([people.column("role"), Arel.count(people.column("id"))])
    sql, params = query.group(people.column("role")).to_sql()
    Minitest.assert_equal("SELECT \"people\".\"role\", COUNT(\"people\".\"id\") FROM \"people\" GROUP BY \"people\".\"role\"", sql)
  end

  def test_having_uses_expression_nodes_and_binds()
    people = Arel.table("people")
    count = Arel.count(people.column("id"))
    query = Arel.from(people).project([people.column("role"), count])
    sql, params = query.group(people.column("role")).having(count.gt(2)).to_sql()
    Minitest.assert_equal("SELECT \"people\".\"role\", COUNT(\"people\".\"id\") FROM \"people\" GROUP BY \"people\".\"role\" HAVING COUNT(\"people\".\"id\") > ?", sql)
    Minitest.assert_equal(2, params[0])
  end

  suite = Minitest.new()
  suite.test("BETWEEN predicates", test_between_predicates_bind_both_bounds)
  suite.test("LIKE predicates", test_like_predicates_are_bound)
  suite.test("function and aggregate projections", test_function_and_aggregate_projections)
  suite.test("GROUP BY", test_group_by_accepts_expression_arrays)
  suite.test("HAVING", test_having_uses_expression_nodes_and_binds)
  suite.run()
end

run_tests()
