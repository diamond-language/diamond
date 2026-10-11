require "../../lib/minitest"
require "../../packages/arel/lib/arel"

def query_with_flags(quoted, bind, distinct)
  Arel::Query.new("people", [], [], nil, nil, [], quoted, bind, nil, distinct)
end

def run_tests()
  def test_boolean_flags_and_default()
    query = query_with_flags(false, false, false)
    Minitest.assert_equal(false, query.quoted_identifiers())
    Minitest.assert_equal(false, query.bind_limits())
    Minitest.assert_equal(false, query.distinct_value())
    query = query_with_flags(true, true, true)
    Minitest.assert_equal(true, query.quoted_identifiers())
    Minitest.assert_equal(true, query.bind_limits())
    Minitest.assert_equal(true, query.distinct_value())
    query = Arel::Query.for_table(Arel.table("people"))
    Minitest.assert_equal(false, query.distinct_value())
  end

  def test_invalid_flags_fail_at_entry()
    def invalid_quoted() = query_with_flags(1, true, false)
    def invalid_bind() = query_with_flags(true, nil, false)
    def invalid_distinct() = query_with_flags(true, true, "yes")
    Minitest.assert_raises[TypeError](invalid_quoted)
    Minitest.assert_raises[TypeError](invalid_bind)
    Minitest.assert_raises[TypeError](invalid_distinct)
  end

  suite = Minitest.new()
  suite.test("Boolean flags and default distinct", test_boolean_flags_and_default)
  suite.test("invalid flags fail at constructor entry", test_invalid_flags_fail_at_entry)
  suite.run!()
end
run_tests()
