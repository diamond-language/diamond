require "../../lib/minitest"
require "../../packages/arel/arel"

class TestArelVisitor < ArelSQLiteVisitor
  def quote_identifier(name: String) -> String = "[#{name}]"
  def render_compound(query) -> Array = ["custom compound", []]
  def render_source(query, params: Array) -> String
    "test_source"
  end
end

class NestedTestArelVisitor < ArelSQLiteVisitor
  def render_source(query, params: Array) -> String
    if query.source_query() != nil
      super(query, params)
    else
      "visited_#{query.table_name()}"
    end
  end
end

class WriteTestArelVisitor < ArelSQLiteVisitor
  def quote_identifier(name: String) -> String = "[#{name}]"
  def render_insert(statement) -> Array
    sql, params = super(statement)
    ["custom #{sql}", params]
  end
  def render_update(statement) -> Array
    sql, params = super(statement)
    ["custom #{sql}", params]
  end
  def render_delete(statement) -> Array
    sql, params = super(statement)
    ["custom #{sql}", params]
  end
  def render_expression(expression, params: Array) -> String
    if expression is ArelExcludedAttribute
      "incoming.#{self.quote_identifier(expression.name())}"
    else
      super(expression, params)
    end
  end
end

def run_tests()
  def test_select_accepts_an_explicit_visitor()
    people = Arel.table("people")
    query = Arel.from(people).project(people.column("value"))
    sql, params = query.to_sql(TestArelVisitor.new())
    Minitest.assert_equal("SELECT [people].[value] FROM test_source", sql)
    Minitest.assert_equal(0, params.length())
  end

  def test_derived_queries_inherit_the_explicit_visitor()
    people = Arel.table("people")
    inner = Arel.from(people).project(people.column("name"))
    derived = Arel.table("named")
    outer = Arel.from_subquery(inner, "named").project(derived.column("name"))
    sql, params = outer.to_sql(NestedTestArelVisitor.new())
    Minitest.assert_equal("SELECT \"named\".\"name\" FROM (SELECT \"people\".\"name\" FROM visited_people) AS \"named\"", sql)
  end

  def test_compound_branches_inherit_the_explicit_visitor()
    first = Arel.table("first_values")
    second = Arel.table("second_values")
    left = Arel.from(first).project(first.column("value"))
    right = Arel.from(second).project(second.column("value"))
    sql, params = Arel.union_all(left, right).to_sql(NestedTestArelVisitor.new())
    Minitest.assert_equal("SELECT \"first_values\".\"value\" FROM visited_first_values UNION ALL SELECT \"second_values\".\"value\" FROM visited_second_values", sql)
  end

  def test_cte_bodies_inherit_the_explicit_visitor()
    people = Arel.table("people")
    source = Arel.from(people).project(people.column("name"))
    named = Arel.cte("named")
    query = Arel.from(named).with(named, source)
    sql, params = query.to_sql(NestedTestArelVisitor.new())
    Minitest.assert_equal("WITH \"named\" AS (SELECT \"people\".\"name\" FROM visited_people) SELECT * FROM visited_named", sql)
  end

  def test_insert_expressions_use_the_explicit_visitor()
    inventory = Arel.table("inventory")
    insert = Arel.insert_into(inventory).values({"name": "pens", "qty": 4})
    insert = insert.on_conflict_do_update(["name"], {
      "qty": Arel.expression(Arel.excluded("qty"))
    })
    sql, params = insert.to_sql(WriteTestArelVisitor.new())
    Minitest.assert_equal(true, sql.include?("[qty] = incoming.[qty]"))
    Minitest.assert_equal(true, sql.include?("custom INSERT INTO [inventory]"))
    Minitest.assert_equal("pens|4", params.join("|"))
  end

  def test_update_expressions_use_the_explicit_visitor()
    inventory = Arel.table("inventory")
    update = Arel.update(inventory).set({
      "qty": Arel.expression(Arel.excluded("qty"))
    }).where(inventory.column("id").eq(7))
    sql, params = update.to_sql(WriteTestArelVisitor.new())
    Minitest.assert_equal("custom UPDATE [inventory] SET [qty] = incoming.[qty] WHERE [inventory].[id] = ?", sql)
    Minitest.assert_equal("7", params.join("|"))
  end

  def test_delete_returning_uses_the_explicit_visitor()
    inventory = Arel.table("inventory")
    deletion = Arel.delete_from(inventory).where(
      inventory.column("archived").eq(true)).returning(Arel.excluded("id"))
    sql, params = deletion.to_sql(WriteTestArelVisitor.new())
    Minitest.assert_equal("custom DELETE FROM [inventory] WHERE [inventory].[archived] = ? RETURNING incoming.[id]", sql)
    Minitest.assert_equal("true", params.join("|"))
  end

  def test_select_execution_accepts_an_explicit_visitor()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE values_table (value INTEGER)")
    db.execute("INSERT INTO values_table VALUES (1), (2)")
    values = Arel.table("values_table")
    query = Arel.from(values).project(values.column("value"))
    visitor = ArelSQLiteVisitor.new()
    Minitest.assert_equal(2, query.to_a(db, visitor).length())
    Minitest.assert_equal(2, query.count(db, visitor))
    db.close()
  end

  def test_compound_execution_accepts_an_explicit_visitor()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE values_table (value INTEGER)")
    db.execute("INSERT INTO values_table VALUES (1)")
    values = Arel.table("values_table")
    branch = Arel.from(values).project(values.column("value"))
    rows = Arel.union_all(branch, branch).to_a(db, ArelSQLiteVisitor.new())
    Minitest.assert_equal(2, rows.length())
    db.close()
  end

  def test_write_execution_accepts_an_explicit_visitor()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE items (id INTEGER, qty INTEGER)")
    items = Arel.table("items")
    visitor = ArelSQLiteVisitor.new()
    insert = Arel.insert_into(items).values({"id": 1, "qty": 2})
    Minitest.assert_equal(1, insert.execute(db, visitor))
    update = Arel.update(items).set({"qty": 3}).where(items.column("id").eq(1))
    Minitest.assert_equal(1, update.execute(db, visitor))
    deletion = Arel.delete_from(items).where(items.column("id").eq(1))
    Minitest.assert_equal(1, deletion.execute(db, visitor))
    db.close()
  end

  def test_arel_render_is_the_common_visitor_entry_point()
    people = Arel.table("people")
    branch = Arel.from(people).project(people.column("value"))
    query = Arel.union(branch, branch)
    sql, params = Arel.render(query, TestArelVisitor.new())
    Minitest.assert_equal("custom compound", sql)
    Minitest.assert_equal(0, params.length())
  end

  suite = Minitest.new()
  suite.test("explicit SELECT visitor", test_select_accepts_an_explicit_visitor)
  suite.test("nested SELECT visitor", test_derived_queries_inherit_the_explicit_visitor)
  suite.test("compound visitor", test_compound_branches_inherit_the_explicit_visitor)
  suite.test("CTE visitor", test_cte_bodies_inherit_the_explicit_visitor)
  suite.test("INSERT visitor", test_insert_expressions_use_the_explicit_visitor)
  suite.test("UPDATE visitor", test_update_expressions_use_the_explicit_visitor)
  suite.test("DELETE visitor", test_delete_returning_uses_the_explicit_visitor)
  suite.test("SELECT execution visitor", test_select_execution_accepts_an_explicit_visitor)
  suite.test("compound execution visitor", test_compound_execution_accepts_an_explicit_visitor)
  suite.test("write execution visitor", test_write_execution_accepts_an_explicit_visitor)
  suite.test("common render entry point", test_arel_render_is_the_common_visitor_entry_point)
  suite.run!()
end

run_tests()
