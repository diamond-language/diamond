require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_single_cte_renders_and_binds_before_main_query()
    people = Arel.table("people")
    active = Arel.from(people).where(people.column("active").eq(true))
    active = active.order(people.column("age").desc()).take(3)
    active_people = Arel.table("active\"people")
    query = Arel.from(active_people).with("active\"people", active)
    query = query.where(active_people.column("age").gteq(18))
    sql, params = query.to_sql()
    Minitest.assert_equal("WITH \"active\"\"people\" AS (SELECT * FROM \"people\" WHERE \"people\".\"active\" = ? ORDER BY \"people\".\"age\" DESC LIMIT ?) SELECT * FROM \"active\"\"people\" WHERE \"active\"\"people\".\"age\" >= ?", sql)
    Minitest.assert_equal(true, params[0])
    Minitest.assert_equal("3|18", [params[1], params[2]].join("|"))
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE people (age INTEGER, active INTEGER)")
    db.execute("INSERT INTO people VALUES (30, 1), (20, 1), (10, 1), (40, 0)")
    rows = query.to_a(db)
    Minitest.assert_equal(2, rows.length())
    Minitest.assert_equal(30, rows[0]["age"])
    db.close()
  end

  def test_multiple_ctes_preserve_declaration_and_bind_order()
    people = Arel.table("people")
    orders = Arel.table("orders")
    adults = Arel.from(people).where(people.column("age").gteq(18))
    large_orders = Arel.from(orders).where(orders.column("total").gt(100))
    adults_ref = Arel.table("adults")
    orders_ref = Arel.table("large_orders")
    query = Arel.from(adults_ref).with("adults", adults)
    query = query.with("large_orders", large_orders).cross_join(orders_ref)
    sql, params = query.to_sql()
    Minitest.assert_equal("WITH \"adults\" AS (SELECT * FROM \"people\" WHERE \"people\".\"age\" >= ?), \"large_orders\" AS (SELECT * FROM \"orders\" WHERE \"orders\".\"total\" > ?) SELECT * FROM \"adults\" CROSS JOIN \"large_orders\"", sql)
    Minitest.assert_equal(18, params[0])
    Minitest.assert_equal(100, params[1])
  end

  def test_duplicate_cte_names_are_rejected()
    people = Arel.table("people")
    source = Arel.from(people)
    message = nil
    begin
      Arel.from(Arel.table("active")).with("active", source).with("active", source)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("duplicate CTE name", message)
    message = nil
    begin
      Arel.from(Arel.table("active")).with("active", source).with("ACTIVE", source)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("duplicate CTE name", message)
    message = nil
    begin
      Arel.from(Arel.table("active")).with("", source).to_sql()
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("SQL identifier cannot be empty", message)
  end

  def test_compound_query_can_be_a_cte_body()
    current = Arel.table("current_items")
    archived = Arel.table("archived_items")
    combined = Arel.union_all(
      Arel.from(current).project(current.column("id")),
      Arel.from(archived).project(archived.column("id")))
    all_items = Arel.table("all_items")
    sql, params = Arel.from(all_items).with("all_items", combined).to_sql()
    Minitest.assert_equal("WITH \"all_items\" AS (SELECT \"current_items\".\"id\" FROM \"current_items\" UNION ALL SELECT \"archived_items\".\"id\" FROM \"archived_items\") SELECT * FROM \"all_items\"", sql)
  end

  def test_recursive_cte_marks_with_clause()
    seeds = Arel.table("seed_values")
    numbers = Arel.table("numbers")
    anchor = Arel.from(seeds).project(seeds.column("value"))
    step = Arel.from(numbers).project(numbers.column("value"))
    body = Arel.union_all(anchor, step)
    sql, params = Arel.from(numbers).with_recursive("numbers", body).to_sql()
    Minitest.assert_equal("WITH RECURSIVE \"numbers\" AS (SELECT \"seed_values\".\"value\" FROM \"seed_values\" UNION ALL SELECT \"numbers\".\"value\" FROM \"numbers\") SELECT * FROM \"numbers\"", sql)
  end

  def test_recursive_cte_executes_against_sqlite()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE seed_values (value INTEGER)")
    db.execute("INSERT INTO seed_values VALUES (1)")
    seeds = Arel.table("seed_values")
    numbers = Arel.table("numbers")
    anchor = Arel.from(seeds).project(seeds.column("value"))
    step = Arel.from(numbers).project(Arel.sql("value + 1"))
    step = step.where(numbers.column("value").lt(3))
    body = Arel.union_all(anchor, step)
    rows = Arel.from(numbers).with_recursive("numbers", body).to_a(db)
    Minitest.assert_equal(3, rows.length())
    Minitest.assert_equal(1, rows[0]["value"])
    Minitest.assert_equal(3, rows[2]["value"])
    db.close()
  end

  def test_named_cte_relation_builds_scoped_attributes()
    active = Arel.cte("active\"people")
    query = Arel.from(active).project(active.column("name"))
    sql, params = query.to_sql()
    Minitest.assert_equal("active\"people", active.name())
    Minitest.assert_equal("SELECT \"active\"\"people\".\"name\" FROM \"active\"\"people\"", sql)
  end

  def test_cte_relation_builds_recursive_union_body()
    seeds = Arel.table("seeds")
    numbers = Arel.cte("numbers")
    anchor = Arel.from(seeds).project(seeds.column("value"))
    step = Arel.from(numbers).project(Arel.sql("value + 1"))
    body = numbers.recursive_body(anchor, step)
    sql, params = body.to_sql()
    Minitest.assert_equal("SELECT \"seeds\".\"value\" FROM \"seeds\" UNION ALL SELECT value + 1 FROM \"numbers\"", sql)
  end

  def test_recursive_declaration_accepts_its_relation()
    seeds = Arel.table("seeds")
    numbers = Arel.cte("numbers")
    anchor = Arel.from(seeds).project(seeds.column("value"))
    step = Arel.from(numbers).project(Arel.sql("value + 1"))
    body = numbers.recursive_body(anchor, step)
    sql, params = Arel.from(numbers).with_recursive(numbers, body).to_sql()
    Minitest.assert_equal("WITH RECURSIVE \"numbers\" AS (SELECT \"seeds\".\"value\" FROM \"seeds\" UNION ALL SELECT value + 1 FROM \"numbers\") SELECT * FROM \"numbers\"", sql)
  end

  def test_recursive_body_requires_self_reference()
    seeds = Arel.table("seeds")
    other = Arel.table("other")
    numbers = Arel.cte("numbers")
    anchor = Arel.from(seeds).project(seeds.column("value"))
    invalid_step = Arel.from(other).project(other.column("value"))
    message = nil
    begin
      numbers.recursive_body(anchor, invalid_step)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("recursive branch must reference its CTE relation", message)
  end

  def test_recursive_body_rejects_self_referencing_anchor()
    numbers = Arel.cte("numbers")
    anchor = Arel.from(numbers).project(numbers.column("value"))
    step = Arel.from(numbers).project(Arel.sql("value + 1"))
    message = nil
    begin
      numbers.recursive_body(anchor, step)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("recursive CTE anchor cannot reference itself", message)
  end

  def test_ordinary_declaration_accepts_its_relation()
    people = Arel.table("people")
    active = Arel.cte("active")
    source = Arel.from(people).where(people.column("active").eq(true))
    sql, params = Arel.from(active).with(active, source).to_sql()
    Minitest.assert_equal("WITH \"active\" AS (SELECT * FROM \"people\" WHERE \"people\".\"active\" = ?) SELECT * FROM \"active\"", sql)
    Minitest.assert_equal(true, params[0])
  end

  def test_recursive_and_ordinary_ctes_compose()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE source_values (value INTEGER)")
    db.execute("INSERT INTO source_values VALUES (1), (10)")
    source = Arel.table("source_values")
    seeds = Arel.cte("seeds")
    numbers = Arel.cte("numbers")
    seed_query = Arel.from(source).project(source.column("value"))
    seed_query = seed_query.where(source.column("value").lt(5))
    anchor = Arel.from(seeds).project(seeds.column("value"))
    step = Arel.from(numbers).project(Arel.sql("value + 1"))
    step = step.where(numbers.column("value").lt(3))
    body = numbers.recursive_body(anchor, step)
    query = Arel.from(numbers).with(seeds, seed_query)
    query = query.with_recursive(numbers, body)
    rows = query.to_a(db)
    Minitest.assert_equal(3, rows.length())
    Minitest.assert_equal(1, rows[0]["value"])
    Minitest.assert_equal(3, rows[2]["value"])
    db.close()
  end

  suite = Minitest.new()
  suite.test("single CTE", test_single_cte_renders_and_binds_before_main_query)
  suite.test("multiple CTEs", test_multiple_ctes_preserve_declaration_and_bind_order)
  suite.test("duplicate CTE names", test_duplicate_cte_names_are_rejected)
  suite.test("compound CTE body", test_compound_query_can_be_a_cte_body)
  suite.test("recursive CTE", test_recursive_cte_marks_with_clause)
  suite.test("recursive CTE execution", test_recursive_cte_executes_against_sqlite)
  suite.test("named CTE relation", test_named_cte_relation_builds_scoped_attributes)
  suite.test("recursive CTE body helper", test_cte_relation_builds_recursive_union_body)
  suite.test("recursive CTE relation declaration", test_recursive_declaration_accepts_its_relation)
  suite.test("recursive self-reference validation", test_recursive_body_requires_self_reference)
  suite.test("recursive anchor validation", test_recursive_body_rejects_self_referencing_anchor)
  suite.test("ordinary CTE relation declaration", test_ordinary_declaration_accepts_its_relation)
  suite.test("mixed recursive CTEs", test_recursive_and_ordinary_ctes_compose)
  suite.run!()
end

run_tests()
