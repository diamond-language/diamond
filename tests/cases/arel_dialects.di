require "../../lib/minitest"
require "../../packages/arel/arel"

class PortableTestVisitor < ArelSQLiteVisitor
  def visitor_name() = "portable-test"
  def supports_extension?(name: String) = false
end

def run_tests()
  def test_visitors_report_unsupported_extensions()
    visitor = PortableTestVisitor.new()
    message = nil
    begin
      visitor.require_extension("example extension")
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support example extension", message)
  end

  def test_excluded_attributes_are_dialect_extensions()
    items = Arel.table("items")
    update = Arel.update(items).set({
      "qty": Arel.expression(Arel.excluded("qty"))
    }).all()
    message = nil
    begin
      update.to_sql(PortableTestVisitor.new())
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support excluded-row attributes", message)
  end

  suite = Minitest.new()
  suite.test("visitor extension protocol", test_visitors_report_unsupported_extensions)
  suite.test("excluded extension", test_excluded_attributes_are_dialect_extensions)
  suite.run()
end

run_tests()
