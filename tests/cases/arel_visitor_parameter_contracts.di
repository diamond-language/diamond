require "../../lib/minitest"
require "../../packages/arel/lib/arel"

# Keep the caller dynamic so entry guards receive values not known statically.
def render_checked_pagination(visitor, limit, offset, params, bind)
  visitor.render_pagination(limit, offset, params, bind)
end

def pagination_visitors()
  [Arel::SQLiteVisitor.new(), Arel::PostgreSQLVisitor.new(),
    Arel::MariaDBVisitor.new(), Arel::MySQLVisitor.new()]
end

class CustomPaginationVisitor < Arel::SQLiteVisitor
  def render_pagination(limit, offset, params: Array, bind = true) -> String
    if limit == "custom"
      " LIMIT CUSTOM"
    else
      super(limit, offset, params, bind)
    end
  end
end

def run_tests()
  def test_nullable_zero_and_binding_contracts()
    visitors = pagination_visitors()
    index = 0
    while index < visitors.length()
      visitor = visitors[index]
      params = ["existing"]
      Minitest.assert_equal("", visitor.render_pagination(nil, nil, params))
      Minitest.assert_equal(["existing"], params)
      Minitest.assert_equal(" LIMIT ? OFFSET ?",
        visitor.render_pagination(0, 0, params))
      Minitest.assert_equal(["existing", 0, 0], params)
      params = []
      Minitest.assert_equal(" LIMIT 0 OFFSET 0",
        render_checked_pagination(visitor, 0, 0, params, false))
      Minitest.assert_equal([], params)
      index += 1
    end
  end

  def test_offset_only_keeps_each_dialect_sentinel()
    visitors = pagination_visitors()
    expected = [" LIMIT -1 OFFSET ?", " OFFSET ?",
      " LIMIT 18446744073709551615 OFFSET ?",
      " LIMIT 18446744073709551615 OFFSET ?"]
    index = 0
    while index < visitors.length()
      visitor = visitors[index]
      params = []
      Minitest.assert_equal(expected[index],
        visitor.render_pagination(nil, 0, params))
      Minitest.assert_equal([0], params)
      index += 1
    end
  end

  def test_invalid_parameters_do_not_mutate_binds()
    visitors = pagination_visitors()
    index = 0
    while index < visitors.length()
      visitor = visitors[index]
      params = ["existing"]
      def bad_limit() = render_checked_pagination(visitor, "1", nil, params, true)
      def bad_offset() = render_checked_pagination(visitor, 1, false, params, true)
      def bad_binding() = render_checked_pagination(visitor, 1, 0, params, nil)
      Minitest.assert_raises[TypeError](bad_limit)
      Minitest.assert_raises[TypeError](bad_offset)
      Minitest.assert_raises[TypeError](bad_binding)
      Minitest.assert_equal(["existing"], params)
      index += 1
    end
  end

  def test_custom_pagination_override_remains_dynamic()
    visitor = CustomPaginationVisitor.new()
    Minitest.assert_equal(" LIMIT CUSTOM",
      render_checked_pagination(visitor, "custom", nil, [], true))
    Minitest.assert_equal(" LIMIT 0",
      render_checked_pagination(visitor, 0, nil, [], false))
  end

  suite = Minitest.new()
  suite.test("nullable zero and binding contracts", test_nullable_zero_and_binding_contracts)
  suite.test("offset-only dialect SQL", test_offset_only_keeps_each_dialect_sentinel)
  suite.test("invalid parameters preserve binds", test_invalid_parameters_do_not_mutate_binds)
  suite.test("custom pagination override", test_custom_pagination_override_remains_dynamic)
  suite.run!()
end

run_tests()
