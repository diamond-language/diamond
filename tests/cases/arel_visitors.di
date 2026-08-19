require "../../lib/minitest"
require "../../packages/arel/arel"

class TestArelVisitor < ArelSQLiteVisitor
  def render_source(query, params: Array) -> String
    "test_source"
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

  suite = Minitest.new()
  suite.test("explicit SELECT visitor", test_select_accepts_an_explicit_visitor)
  suite.run()
end

run_tests()
