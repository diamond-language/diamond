require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_single_cte_renders_and_binds_before_main_query()
    people = Arel.table("people")
    active = Arel.from(people).where(people.column("active").eq(true))
    active_people = Arel.table("active_people")
    query = Arel.from(active_people).with("active_people", active)
    query = query.where(active_people.column("age").gteq(18))
    sql, params = query.to_sql()
    Minitest.assert_equal("WITH \"active_people\" AS (SELECT * FROM \"people\" WHERE \"people\".\"active\" = ?) SELECT * FROM \"active_people\" WHERE \"active_people\".\"age\" >= ?", sql)
    Minitest.assert_equal(true, params[0])
    Minitest.assert_equal(18, params[1])
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

  suite = Minitest.new()
  suite.test("single CTE", test_single_cte_renders_and_binds_before_main_query)
  suite.test("multiple CTEs", test_multiple_ctes_preserve_declaration_and_bind_order)
  suite.test("duplicate CTE names", test_duplicate_cte_names_are_rejected)
  suite.test("compound CTE body", test_compound_query_can_be_a_cte_body)
  suite.run()
end

run_tests()
