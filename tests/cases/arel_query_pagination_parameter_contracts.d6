require "../../lib/minitest"
require "../../packages/arel/lib/arel"

def query_with_pagination(limit, offset)
  Arel::Query.new("people", [], [], limit, offset,
                 [Arel::RawSql.new("*", [])], true, true)
end

def run_tests()
  def test_nullable_zero_and_binding_contracts()
    query = query_with_pagination(nil, nil)
    Minitest.assert_equal(nil, query.limit_value())
    Minitest.assert_equal(nil, query.offset_value())
    Minitest.assert_equal(["SELECT * FROM \"people\"", []], query.to_sql())
    query = query_with_pagination(0, 2)
    Minitest.assert_equal(["SELECT * FROM \"people\" LIMIT ? OFFSET ?", [0, 2]], query.to_sql())
    query = query_with_pagination(nil, 3)
    Minitest.assert_equal(["SELECT * FROM \"people\" LIMIT -1 OFFSET ?", [3]], query.to_sql())
    query = query_with_pagination(-1, -2)
    Minitest.assert_equal(-1, query.limit_value())
    Minitest.assert_equal(-2, query.offset_value())
    big = 9223372036854775807 + 1
    query = query_with_pagination(big, big)
    Minitest.assert_equal(big, query.limit_value())
    Minitest.assert_equal(big, query.offset_value())
    sql, params = query.to_sql()
    Minitest.assert_equal([big, big], params)
  end

  def test_invalid_pagination_fails_at_entry()
    def float_limit() = query_with_pagination(1.5, nil)
    def string_limit() = query_with_pagination("1", nil)
    def false_offset() = query_with_pagination(nil, false)
    def array_offset() = query_with_pagination(nil, [])
    Minitest.assert_raises[TypeError](float_limit)
    Minitest.assert_raises[TypeError](string_limit)
    Minitest.assert_raises[TypeError](false_offset)
    Minitest.assert_raises[TypeError](array_offset)
  end

  suite = Minitest.new()
  suite.test("nullable and promoted integers preserve pagination", test_nullable_zero_and_binding_contracts)
  suite.test("invalid pagination fails at constructor entry", test_invalid_pagination_fails_at_entry)
  suite.run!()
end
run_tests()
