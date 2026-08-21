require "../../lib/minitest"
require "../../packages/arel/lib/arel"

class PortableTestVisitor < Arel::Visitor
  def visitor_name() = "portable-test"
  def quote_identifier(name: String) -> String = arel_quote_identifier(name)
  def supports_extension?(name: String) = false
  def render_pagination(limit_value, offset_value, params: Array,
                        bind_values = true) -> String
    sql = ""
    if limit_value != nil
      sql = " LIMIT #{limit_value}"
    end
    if offset_value != nil
      sql = sql + " OFFSET #{offset_value}"
    end
    sql
  end
  def render_literal(value) -> String
    if value is Bool
      if value
        "1"
      else
        "0"
      end
    else
      super(value)
    end
  end
end

def run_tests()
  def test_visitors_report_unsupported_extensions()
    visitor = PortableTestVisitor.new()
    Minitest.assert_equal("portable-test", visitor.visitor_name())
    Minitest.assert_equal(false, visitor.supports_extension?("returning clauses"))
    Minitest.assert_equal("SQLite", Arel::SQLiteVisitor.new().visitor_name())
    Minitest.assert_equal(true,
      Arel::SQLiteVisitor.new().supports_extension?("returning clauses"))
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
    message = nil
    begin
      Arel.insert_into(items).values({"name": "pens"}).returning(
        items.column("id")).to_sql(PortableTestVisitor.new())
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support returning clauses", message)
    message = nil
    begin
      Arel.delete_from(items).all().returning(items.column("id")).to_sql(
        PortableTestVisitor.new())
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support returning clauses", message)
    message = nil
    begin
      Arel.update(items).set({"name": "pencils"}).all().returning(
        items.column("id")).to_sql(PortableTestVisitor.new())
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support returning clauses", message)
    message = nil
    begin
      Arel.from(items).order(items.column("name").asc().nulls_last()).to_sql(
        PortableTestVisitor.new())
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support explicit NULL ordering", message)
    source = Arel.from(items).project(items.column("id"))
    message = nil
    begin
      Arel.update(items).with("selected", source).set({"qty": 2}).all().to_sql(
        PortableTestVisitor.new())
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support write CTEs", message)
    message = nil
    begin
      Arel.insert_into(items).with("selected", source).values({"qty": 2}).to_sql(
        PortableTestVisitor.new())
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support write CTEs", message)
    message = nil
    begin
      Arel.delete_from(items).with("selected", source).all().to_sql(
        PortableTestVisitor.new())
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support write CTEs", message)
  end

  def test_portable_select_nodes_render_without_extensions()
    people = Arel.table("people")
    query = Arel.from(people).project([
      people.column("name"), Arel.as(people.column("score").add(1), "next_score"),
      Arel.as(Arel.literal(true), "enabled")
    ])
    query = query.where(people.column("active").eq(true))
    query = query.order(people.column("name").asc()).take(5)
    sql, params = query.to_sql(PortableTestVisitor.new())
    Minitest.assert_equal("SELECT \"people\".\"name\", (\"people\".\"score\" + ?) AS \"next_score\", 1 AS \"enabled\" FROM \"people\" WHERE \"people\".\"active\" = ? ORDER BY \"people\".\"name\" ASC LIMIT 5", sql)
    Minitest.assert_equal(1, params[0])
    Minitest.assert_equal(true, params[1])
    Minitest.assert_equal(2, params.length())
    message = nil
    visitor = PortableTestVisitor.new()
    begin
      Arel.from(people).project(Arel.table("accounts").column("id")).to_sql(
        visitor)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("attribute belongs to a relation outside this query", message)
    accounts = Arel.table("accounts")
    sql, params = Arel.update(accounts).set({
      "score": Arel.expression(accounts.column("score").add(1))
    }).all().to_sql(visitor)
    Minitest.assert_equal("UPDATE \"accounts\" SET \"score\" = (\"accounts\".\"score\" + ?)", sql)
    Minitest.assert_equal("1", params.join("|"))
  end

  def test_portable_write_nodes_render_without_extensions()
    items = Arel.table("items")
    visitor = PortableTestVisitor.new()
    insert_sql, insert_params = Arel.insert_into(items).values({
      "name": "pens", "qty": Arel.expression(items.column("qty").add(1))
    }).to_sql(visitor)
    update = Arel.update(items).set({"qty": 3})
    update_sql, update_params = update.where(items.column("name").eq("pens")).to_sql(visitor)
    deletion = Arel.delete_from(items).where(items.column("qty").eq(0))
    delete_sql, delete_params = deletion.to_sql(visitor)
    Minitest.assert_equal("INSERT INTO \"items\" (\"name\", \"qty\") VALUES (?, (\"items\".\"qty\" + ?))", insert_sql)
    Minitest.assert_equal("UPDATE \"items\" SET \"qty\" = ? WHERE \"items\".\"name\" = ?", update_sql)
    Minitest.assert_equal("DELETE FROM \"items\" WHERE \"items\".\"qty\" = ?", delete_sql)
    Minitest.assert_equal(2, insert_params.length())
    Minitest.assert_equal(2, update_params.length())
    Minitest.assert_equal(1, delete_params.length())
  end

  def test_portable_compounds_and_ctes_render_without_extensions()
    current = Arel.table("current_items")
    archived = Arel.table("archived_items")
    left = Arel.from(current).project(current.column("id"))
    right = Arel.from(archived).project(archived.column("id"))
    combined = Arel.union_all(left, right).take(4).skip(1)
    all_items = Arel.cte("all_items")
    query = Arel.from(all_items).with(all_items, combined)
    sql, params = query.to_sql(PortableTestVisitor.new())
    Minitest.assert_equal("WITH \"all_items\" AS (SELECT \"current_items\".\"id\" FROM \"current_items\" UNION ALL SELECT \"archived_items\".\"id\" FROM \"archived_items\" LIMIT 4 OFFSET 1) SELECT * FROM \"all_items\"", sql)
    Minitest.assert_equal(0, params.length())
  end

  def test_extension_nodes_report_their_capability_names()
    message = nil
    begin
      Arel.from("items").project(Arel.integer_operator(
        Arel.table("items").column("mask"), "&", 1)).to_sql(PortableTestVisitor.new())
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support integer bitwise operators", message)
    message = nil
    begin
      Arel.from(Arel.cte("items")).with_recursive(
        "items", Arel.from("source")).to_sql(PortableTestVisitor.new())
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("portable-test visitor does not support recursive CTEs", message)
  end

  suite = Minitest.new()
  suite.test("visitor extension protocol", test_visitors_report_unsupported_extensions)
  suite.test("excluded extension", test_excluded_attributes_are_dialect_extensions)
  suite.test("partial conflict extension", test_partial_conflict_targets_are_dialect_extensions)
  suite.test("upsert extension", test_upsert_actions_are_dialect_extensions)
  suite.test("default values extension", test_default_values_are_a_dialect_extension)
  suite.test("portable SELECT nodes", test_portable_select_nodes_render_without_extensions)
  suite.test("portable write nodes", test_portable_write_nodes_render_without_extensions)
  suite.test("portable compounds and CTEs", test_portable_compounds_and_ctes_render_without_extensions)
  suite.test("extension node metadata", test_extension_nodes_report_their_capability_names)
  suite.run!()
end

run_tests()
