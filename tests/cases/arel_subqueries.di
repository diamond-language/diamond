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

  def test_exists_and_not_exists_are_predicates()
    people = Arel.table("people")
    inner = Arel.from(people).where(people.column("active").eq(true))
    outer_table = Arel.table("settings")
    predicate = Arel.exists(inner).and_also(Arel.not_exists(inner).not_())
    sql, params = Arel.from(outer_table).where(predicate).to_sql()
    Minitest.assert_equal("SELECT * FROM \"settings\" WHERE (EXISTS (SELECT * FROM \"people\" WHERE \"people\".\"active\" = ?) AND EXISTS (SELECT * FROM \"people\" WHERE \"people\".\"active\" = ?))", sql)
    Minitest.assert_equal(2, params.length())
    Minitest.assert_equal(true, params[0])
  end

  suite = Minitest.new()
  suite.test("FROM subquery", test_subquery_can_be_used_as_from_source)
  suite.test("EXISTS predicates", test_exists_and_not_exists_are_predicates)
  suite.run()
end

run_tests()
