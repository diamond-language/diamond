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

  def test_partial_conflict_targets_are_dialect_extensions()
    items = Arel.table("items")
    target = Arel.conflict_target(["name"])
    target = target.where(target.column("active").eq(Arel.literal(1)))
    insert = Arel.insert_into(items).values({"name": "pens", "active": 1})
    insert = insert.on_conflict_do_nothing(target)
    message = nil
    begin
      insert.to_sql(PortableTestVisitor.new())
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support conflict-target predicates", message)
  end

  def test_upsert_actions_are_dialect_extensions()
    items = Arel.table("items")
    insert = Arel.insert_into(items).values({"name": "pens"})
    insert = insert.on_conflict_do_nothing(["name"])
    message = nil
    begin
      insert.to_sql(PortableTestVisitor.new())
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support upsert conflict actions", message)
  end

  def test_default_values_are_a_dialect_extension()
    items = Arel.table("items")
    message = nil
    begin
      Arel.insert_into(items).default_values().to_sql(PortableTestVisitor.new())
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support insert default values", message)
  end

  def test_portable_select_nodes_render_without_extensions()
    people = Arel.table("people")
    query = Arel.from(people).project([
      people.column("name"), Arel.as(people.column("score").add(1), "next_score")
    ])
    query = query.where(people.column("active").eq(true))
    query = query.order(people.column("name").asc()).take(5)
    sql, params = query.to_sql(PortableTestVisitor.new())
    Minitest.assert_equal("SELECT \"people\".\"name\", (\"people\".\"score\" + ?) AS \"next_score\" FROM \"people\" WHERE \"people\".\"active\" = ? ORDER BY \"people\".\"name\" ASC LIMIT ?", sql)
    Minitest.assert_equal(1, params[0])
    Minitest.assert_equal(true, params[1])
    Minitest.assert_equal(5, params[2])
  end

  suite = Minitest.new()
  suite.test("visitor extension protocol", test_visitors_report_unsupported_extensions)
  suite.test("excluded extension", test_excluded_attributes_are_dialect_extensions)
  suite.test("partial conflict extension", test_partial_conflict_targets_are_dialect_extensions)
  suite.test("upsert extension", test_upsert_actions_are_dialect_extensions)
  suite.test("default values extension", test_default_values_are_a_dialect_extension)
  suite.test("portable SELECT nodes", test_portable_select_nodes_render_without_extensions)
  suite.run()
end

run_tests()
