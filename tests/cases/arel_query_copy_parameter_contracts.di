require "../../lib/minitest"
require "../../packages/arel/lib/arel"

def copy_checked(query, predicates, orderings, limit, offset, projections)
  query.copy(predicates, orderings, limit, offset, projections)
end

def run_tests()
  def test_nullable_limits_and_plain_collections()
    query = Arel::Query.for_table(Arel.table("people"))
    copied = copy_checked(query, [], [], 0, nil, ["name"])
    Minitest.assert_equal(0, copied.limit_value())
    Minitest.assert_equal(nil, copied.offset_value())
    Minitest.assert_equal(["name"], copied.projections())
    Minitest.assert_equal(nil, query.limit_value())
    big = 9223372036854775807 + 1
    copied = copy_checked(query, [], [], nil, big, [])
    Minitest.assert_equal(big, copied.offset_value())
    copied.predicates().push("still a plain array")
    Minitest.assert_equal(1, copied.predicates().length())
  end

  def test_invalid_parameters_fail_at_entry()
    query = Arel::Query.for_table(Arel.table("people"))
    def bad_predicates() = copy_checked(query, nil, [], nil, nil, [])
    def bad_orderings() = copy_checked(query, [], {}, nil, nil, [])
    def bad_limit() = copy_checked(query, [], [], "one", nil, [])
    def bad_offset() = copy_checked(query, [], [], nil, 1.5, [])
    def bad_projections() = copy_checked(query, [], [], nil, nil, {})
    Minitest.assert_raises[TypeError](bad_predicates)
    Minitest.assert_raises[TypeError](bad_orderings)
    Minitest.assert_raises[TypeError](bad_limit)
    Minitest.assert_raises[TypeError](bad_offset)
    Minitest.assert_raises[TypeError](bad_projections)
  end

  suite = Minitest.new()
  suite.test("copy preserves nullable limits and plain arrays", test_nullable_limits_and_plain_collections)
  suite.test("copy rejects invalid state at entry", test_invalid_parameters_fail_at_entry)
  suite.run!()
end
run_tests()
