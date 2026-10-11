require "../../lib/minitest"
require "../../packages/arel/lib/arel"

def query_with_identity(name, table_alias)
  Arel::Query.new(name, [], [], nil, nil, [Arel::RawSql.new("*", [])],
                 true, true, table_alias)
end

def run_tests()
  def test_names_nullable_aliases_and_rendering()
    query = query_with_identity("people", nil)
    Minitest.assert_equal("people", query.table_name())
    Minitest.assert_equal(nil, query.table_alias())
    Minitest.assert_equal("people", query.base_reference_name())
    Minitest.assert_equal(["SELECT * FROM \"people\"", []], query.to_sql())
    query = query_with_identity("people", "p")
    Minitest.assert_equal("p", query.base_reference_name())
    Minitest.assert_equal(["SELECT * FROM \"people\" AS \"p\"", []], query.to_sql())
    query = query_with_identity("quoted\"name", "")
    Minitest.assert_equal("quoted\"name", query.table_name())
    Minitest.assert_equal("", query.table_alias())
    query = Arel::Query.new("people", [], [], nil, nil, [], true, true)
    Minitest.assert_equal(nil, query.table_alias())
  end

  def test_invalid_identity_fails_at_entry()
    def nil_name() = query_with_identity(nil, nil)
    def symbol_name() = query_with_identity(:people, nil)
    def numeric_alias() = query_with_identity("people", 1)
    def false_alias() = query_with_identity("people", false)
    Minitest.assert_raises[TypeError](nil_name)
    Minitest.assert_raises[TypeError](symbol_name)
    Minitest.assert_raises[TypeError](numeric_alias)
    Minitest.assert_raises[TypeError](false_alias)
  end

  suite = Minitest.new()
  suite.test("names and nullable aliases preserve rendering", test_names_nullable_aliases_and_rendering)
  suite.test("invalid identity fails at constructor entry", test_invalid_identity_fails_at_entry)
  suite.run!()
end
run_tests()
