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
    rows = Arel.union_all(branch, branch).skip(1).to_a(db)
    Minitest.assert_equal(1, rows.length())
    Minitest.assert_equal(1, rows[0]["value"])
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

  def test_compound_result_can_be_ordered()
    first = Arel.table("first_values")
    second = Arel.table("second_values")
    left = Arel.from(first).project(first.column("name"))
    right = Arel.from(second).project(second.column("name"))
    ordering = Arel.asc(Arel.sql("name")).nulls_last()
    sql, params = Arel.union_all(left, right).order(ordering).to_sql()
    Minitest.assert_equal("SELECT \"first_values\".\"name\" FROM \"first_values\" UNION ALL SELECT \"second_values\".\"name\" FROM \"second_values\" ORDER BY name ASC NULLS LAST", sql)
  end

  def test_compound_result_can_be_paginated()
    first = Arel.table("first_values")
    second = Arel.table("second_values")
    left = Arel.from(first).project(first.column("id"))
    right = Arel.from(second).project(second.column("id"))
    query = Arel.union_all(left, right).take(10).skip(20)
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT \"first_values\".\"id\" FROM \"first_values\" UNION ALL SELECT \"second_values\".\"id\" FROM \"second_values\" LIMIT ? OFFSET ?", sql)
    Minitest.assert_equal(10, params[0])
    Minitest.assert_equal(20, params[1])
    query = Arel.union_all(left, right).skip(6)
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT \"first_values\".\"id\" FROM \"first_values\" UNION ALL SELECT \"second_values\".\"id\" FROM \"second_values\" LIMIT -1 OFFSET ?", sql)
    Minitest.assert_equal("6", params.join("|"))
  end

  def test_compound_query_can_feed_an_insert()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE current_names (name TEXT)")
    db.execute("CREATE TABLE archived_names (name TEXT)")
    db.execute("CREATE TABLE all_names (name TEXT)")
    db.execute("INSERT INTO current_names VALUES ('new')")
    db.execute("INSERT INTO archived_names VALUES ('old')")
    current = Arel.table("current_names")
    archived = Arel.table("archived_names")
    source = Arel.union_all(
      Arel.from(current).project(current.column("name")),
      Arel.from(archived).project(archived.column("name")))
    target = Arel.table("all_names")
    insert = Arel.insert_into(target).from_query(["name"], source)
    sql, params = insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"all_names\" (\"name\") SELECT \"current_names\".\"name\" FROM \"current_names\" UNION ALL SELECT \"archived_names\".\"name\" FROM \"archived_names\"", sql)
    Minitest.assert_equal(2, insert.execute(db))
    Minitest.assert_equal(2, db.query("SELECT name FROM all_names").length())
    db.close()
  end

  def test_right_nested_compounds_preserve_grouping()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE first_values (value INTEGER)")
    db.execute("CREATE TABLE second_values (value INTEGER)")
    db.execute("CREATE TABLE third_values (value INTEGER)")
    db.execute("INSERT INTO first_values VALUES (1)")
    db.execute("INSERT INTO second_values VALUES (1), (2)")
    db.execute("INSERT INTO third_values VALUES (2)")
    first = Arel.table("first_values")
    second = Arel.table("second_values")
    third = Arel.table("third_values")
    left = Arel.from(first).project(first.column("value"))
    middle = Arel.from(second).project(second.column("value"))
    right = Arel.from(third).project(third.column("value"))
    query = Arel.union(left, Arel.intersect(middle, right))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT \"first_values\".\"value\" FROM \"first_values\" UNION SELECT * FROM (SELECT \"second_values\".\"value\" FROM \"second_values\" INTERSECT SELECT \"third_values\".\"value\" FROM \"third_values\")", sql)
    rows = query.to_a(db)
    Minitest.assert_equal(2, rows.length())
    Minitest.assert_equal(1, rows[0]["value"])
    Minitest.assert_equal(2, rows[1]["value"])
    db.close()
  end

  def test_left_nested_compounds_preserve_local_pagination()
    first = Arel.table("first_values")
    second = Arel.table("second_values")
    third = Arel.table("third_values")
    left = Arel.from(first).project(first.column("value"))
    middle = Arel.from(second).project(second.column("value"))
    right = Arel.from(third).project(third.column("value"))
    limited = Arel.union_all(left, middle).take(2)
    sql, params = Arel.except(limited, right).to_sql()
    Minitest.assert_equal("SELECT * FROM (SELECT \"first_values\".\"value\" FROM \"first_values\" UNION ALL SELECT \"second_values\".\"value\" FROM \"second_values\" LIMIT ?) EXCEPT SELECT \"third_values\".\"value\" FROM \"third_values\"", sql)
    Minitest.assert_equal(2, params[0])
  end

  def test_compounds_reject_unknown_wildcard_shapes()
    people = Arel.table("people")
    message = nil
    begin
      Arel.union(Arel.from(people), Arel.from(people))
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("compound queries require explicit projections", message)
  end

  suite = Minitest.new()
  suite.test("UNION", test_union_combines_queries_and_binds)
  suite.test("UNION ALL", test_union_all_preserves_duplicates_in_sqlite)
  suite.test("INTERSECT", test_intersect_renders_structurally)
  suite.test("EXCEPT", test_except_renders_structurally)
  suite.test("compound projection validation", test_compound_projection_counts_must_match)
  suite.test("compound derived source", test_compound_query_can_be_a_derived_source)
  suite.test("compound ordering", test_compound_result_can_be_ordered)
  suite.test("compound pagination", test_compound_result_can_be_paginated)
  suite.test("compound INSERT source", test_compound_query_can_feed_an_insert)
  suite.test("right-nested compound grouping", test_right_nested_compounds_preserve_grouping)
  suite.test("left-nested compound grouping", test_left_nested_compounds_preserve_local_pagination)
  suite.test("compound wildcard validation", test_compounds_reject_unknown_wildcard_shapes)
  suite.run!()
end

run_tests()
