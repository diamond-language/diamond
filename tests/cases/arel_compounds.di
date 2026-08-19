require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_union_combines_queries_and_binds()
    people = Arel.table("people")
    adults = Arel.from(people).project(people.column("name"))
    adults = adults.where(people.column("age").gteq(18))
    minors = Arel.from(people).project(people.column("name"))
    minors = minors.where(people.column("age").lt(18))
    sql, params = Arel.union(adults, minors).to_sql()
    Minitest.assert_equal("SELECT \"people\".\"name\" FROM \"people\" WHERE \"people\".\"age\" >= ? UNION SELECT \"people\".\"name\" FROM \"people\" WHERE \"people\".\"age\" < ?", sql)
    Minitest.assert_equal(18, params[0])
    Minitest.assert_equal(18, params[1])
  end

  def test_union_all_preserves_duplicates_in_sqlite()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE values_table (value INTEGER)")
    db.execute("INSERT INTO values_table VALUES (?)", [1])
    values = Arel.table("values_table")
    branch = Arel.from(values).project(values.column("value"))
    rows = Arel.union_all(branch, branch).to_a(db)
    Minitest.assert_equal(2, rows.length())
    Minitest.assert_equal(1, rows[0]["value"])
    Minitest.assert_equal(1, rows[1]["value"])
    db.close()
  end

  def test_intersect_renders_structurally()
    left_table = Arel.table("left_values")
    right_table = Arel.table("right_values")
    left = Arel.from(left_table).project(left_table.column("value"))
    right = Arel.from(right_table).project(right_table.column("value"))
    sql, params = Arel.intersect(left, right).to_sql()
    Minitest.assert_equal("SELECT \"left_values\".\"value\" FROM \"left_values\" INTERSECT SELECT \"right_values\".\"value\" FROM \"right_values\"", sql)
  end

  def test_except_renders_structurally()
    all_people = Arel.table("all_people")
    blocked = Arel.table("blocked_people")
    left = Arel.from(all_people).project(all_people.column("id"))
    right = Arel.from(blocked).project(blocked.column("id"))
    sql, params = Arel.except(left, right).to_sql()
    Minitest.assert_equal("SELECT \"all_people\".\"id\" FROM \"all_people\" EXCEPT SELECT \"blocked_people\".\"id\" FROM \"blocked_people\"", sql)
  end

  def test_compound_projection_counts_must_match()
    people = Arel.table("people")
    one = Arel.from(people).project(people.column("id"))
    two = Arel.from(people).project([people.column("id"), people.column("name")])
    message = nil
    begin
      Arel.union(one, two)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("compound queries require equal projection counts", message)
  end

  def test_compound_query_can_be_a_derived_source()
    first = Arel.table("first_values")
    second = Arel.table("second_values")
    left = Arel.from(first).project(first.column("value"))
    right = Arel.from(second).project(second.column("value"))
    combined = Arel.union_all(left, right)
    source = Arel.table("combined")
    outer = Arel.from_subquery(combined, "combined").project(source.column("value"))
    sql, params = outer.to_sql()
    Minitest.assert_equal("SELECT \"combined\".\"value\" FROM (SELECT \"first_values\".\"value\" FROM \"first_values\" UNION ALL SELECT \"second_values\".\"value\" FROM \"second_values\") AS \"combined\"", sql)
  end

  suite = Minitest.new()
  suite.test("UNION", test_union_combines_queries_and_binds)
  suite.test("UNION ALL", test_union_all_preserves_duplicates_in_sqlite)
  suite.test("INTERSECT", test_intersect_renders_structurally)
  suite.test("EXCEPT", test_except_renders_structurally)
  suite.test("compound projection validation", test_compound_projection_counts_must_match)
  suite.test("compound derived source", test_compound_query_can_be_a_derived_source)
  suite.run()
end

run_tests()
