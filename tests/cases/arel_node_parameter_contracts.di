require "../../lib/minitest"
require "../../packages/arel/lib/arel"

def projected_query()
  people = Arel.table("people")
  Arel.from(people).project(people.column("id"))
end

def run_tests()
  def test_cte_recursive_flag_is_checked_at_construction()
    query = projected_query()
    Minitest.assert_equal(false, Arel::Cte.new("c", query).recursive?())
    Minitest.assert_equal(true, Arel::Cte.new("c", query, true).recursive?())
    def nil_flag() = Arel::Cte.new("c", query, nil)
    def string_flag() = Arel::Cte.new("c", query, "yes")
    Minitest.assert_raises[TypeError](nil_flag)
    Minitest.assert_raises[TypeError](string_flag)
  end

  def test_ordering_nulls_is_checked_at_construction()
    column = Arel.table("people").column("id")
    Minitest.assert_equal(nil, Arel::Ordering.new(column, "ASC").nulls())
    Minitest.assert_equal("FIRST", Arel::Ordering.new(column, "ASC", "FIRST").nulls())
    def symbol_nulls() = Arel::Ordering.new(column, "ASC", :first)
    Minitest.assert_raises[TypeError](symbol_nulls)
  end

  def test_compound_query_parts_are_checked_at_construction()
    query = projected_query()
    compound = Arel::CompoundQuery.new(query, "UNION", query, [], 5, 1)
    Minitest.assert_equal(5, compound.limit_value())
    Minitest.assert_equal(1, compound.offset_value())
    def string_limit() = Arel::CompoundQuery.new(query, "UNION", query, [], "5")
    def string_offset() = Arel::CompoundQuery.new(query, "UNION", query, [], nil, "1")
    def hash_orderings() = Arel::CompoundQuery.new(query, "UNION", query, {})
    Minitest.assert_raises[TypeError](string_limit)
    Minitest.assert_raises[TypeError](string_offset)
    Minitest.assert_raises[TypeError](hash_orderings)
  end

  suite = Minitest.new()
  suite.test("Cte recursive flag is checked at construction", test_cte_recursive_flag_is_checked_at_construction)
  suite.test("Ordering nulls is checked at construction", test_ordering_nulls_is_checked_at_construction)
  suite.test("CompoundQuery parts are checked at construction", test_compound_query_parts_are_checked_at_construction)
  suite.run!()
end
run_tests()
