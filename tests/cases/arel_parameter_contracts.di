require "../../lib/minitest"
require "../../packages/arel/lib/arel"

# Dynamic forwarding exercises entry checks before statement rendering.
def delete_with_predicates(value) = Arel::Delete.new(Arel.table("people"), value)
def delete_with_flag(value) = Arel::Delete.new(Arel.table("people"), [], [], value)
def update_with_assignments(value) = Arel::Update.new(Arel.table("people"), value)
def insert_with_rows(value) = Arel::Insert.new(Arel.table("people"), value)

def run_tests()
  def test_defaults_and_false_bound_values()
    table = Arel.table("people")
    Minitest.assert_equal(nil, update_with_assignments(nil).structure()[1])
    Minitest.assert_equal(false, delete_with_flag(false).structure()[3])
    Minitest.assert_equal(true, delete_with_flag(true).structure()[3])
    Minitest.assert_equal([], Arel::Insert.new(table).structure()[1])
    Minitest.assert_equal(nil, Arel::Insert.new(table).structure()[7])
    sql, params = update_with_assignments({"active": false}).all().to_sql()
    Minitest.assert_equal("UPDATE \"people\" SET \"active\" = ?", sql)
    Minitest.assert_equal([false], params)
  end

  def test_invalid_state_fails_at_constructor_entry()
    def invalid_predicates() = delete_with_predicates(nil)
    def invalid_flag() = delete_with_flag(1)
    def invalid_assignments() = update_with_assignments([])
    def invalid_rows() = insert_with_rows({"id": 1})
    Minitest.assert_raises[TypeError](invalid_predicates)
    Minitest.assert_raises[TypeError](invalid_flag)
    Minitest.assert_raises[TypeError](invalid_assignments)
    Minitest.assert_raises[TypeError](invalid_rows)
  end

  suite = Minitest.new()
  suite.test("defaults and false bound values", test_defaults_and_false_bound_values)
  suite.test("runtime constructor checks", test_invalid_state_fails_at_constructor_entry)
  suite.run!()
end

run_tests()
