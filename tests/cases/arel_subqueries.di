require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_subquery_can_be_used_as_from_source()
    people = Arel.table("people")
    inner = Arel.from(people).project(people.column("name"))
    inner = inner.where(people.column("active").eq(true))
    derived = Arel.table("active_people")
    outer = Arel.from_subquery(inner, "active_people")
    outer = outer.project(derived.column("name"))
    sql, params = outer.to_sql()
    Minitest.assert_equal("SELECT \"active_people\".\"name\" FROM (SELECT \"people\".\"name\" FROM \"people\" WHERE \"people\".\"active\" = ?) AS \"active_people\"", sql)
    Minitest.assert_equal(true, params[0])
  end

  suite = Minitest.new()
  suite.test("FROM subquery", test_subquery_can_be_used_as_from_source)
  suite.run()
end

run_tests()
