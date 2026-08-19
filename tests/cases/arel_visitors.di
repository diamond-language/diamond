require "../../lib/minitest"
require "../../packages/arel/arel"

class TestArelVisitor < ArelSQLiteVisitor
  def render_source(query, params: Array) -> String
    "test_source"
  end
end

class NestedTestArelVisitor < ArelSQLiteVisitor
  def render_source(query, params: Array) -> String
    if query.source_query() != nil
      super(query, params)
    else
      "visited_#{query.table_name()}"
    end
  end
end

def run_tests()
  def test_select_accepts_an_explicit_visitor()
    people = Arel.table("people")
    query = Arel.from(people).project(Arel.sql("value"))
    sql, params = query.to_sql(TestArelVisitor.new())
    Minitest.assert_equal("SELECT value FROM test_source", sql)
    Minitest.assert_equal(0, params.length())
  end

  def test_derived_queries_inherit_the_explicit_visitor()
    people = Arel.table("people")
    inner = Arel.from(people).project(people.column("name"))
    derived = Arel.table("named")
    outer = Arel.from_subquery(inner, "named").project(derived.column("name"))
    sql, params = outer.to_sql(NestedTestArelVisitor.new())
    Minitest.assert_equal("SELECT \"named\".\"name\" FROM (SELECT \"people\".\"name\" FROM visited_people) AS \"named\"", sql)
  end

  def test_compound_branches_inherit_the_explicit_visitor()
    first = Arel.table("first_values")
    second = Arel.table("second_values")
    left = Arel.from(first).project(first.column("value"))
    right = Arel.from(second).project(second.column("value"))
    sql, params = Arel.union_all(left, right).to_sql(NestedTestArelVisitor.new())
    Minitest.assert_equal("SELECT \"first_values\".\"value\" FROM visited_first_values UNION ALL SELECT \"second_values\".\"value\" FROM visited_second_values", sql)
  end

  suite = Minitest.new()
  suite.test("explicit SELECT visitor", test_select_accepts_an_explicit_visitor)
  suite.test("nested SELECT visitor", test_derived_queries_inherit_the_explicit_visitor)
  suite.test("compound visitor", test_compound_branches_inherit_the_explicit_visitor)
  suite.run()
end

run_tests()
