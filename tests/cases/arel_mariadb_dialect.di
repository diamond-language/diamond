# Self-contained (no live server) SQL-shape assertions for
# Arel::MariaDBVisitor -- pins down exact rendered SQL for the several
# real per-statement rendering differences this dialect needed (unlike
# PostgreSQLVisitor, which only needed pagination to differ). Real
# execution against a live MariaDB server is covered separately by the
# opt-in packages/arel/test_mariadb_dialect.di/.sh.
require "../../lib/minitest"
require "../../packages/arel/lib/arel"

def run_tests()
  def test_quote_identifier_uses_backticks()
    visitor = Arel::MariaDBVisitor.new()
    people = Arel.table("people")
    sql, params = Arel.from(people).project(people.column("name")).to_sql(visitor)
    Minitest.assert_equal("SELECT `people`.`name` FROM `people`", sql)
    Minitest.assert_equal(0, params.length())
  end

  def test_quote_identifier_escapes_embedded_backtick()
    visitor = Arel::MariaDBVisitor.new()
    people = Arel.table("na`me")
    sql, params = Arel.from(people).to_sql(visitor)
    Minitest.assert_equal("SELECT * FROM `na``me`", sql)
    Minitest.assert_equal(0, params.length())
  end

  def test_quote_identifier_rejects_empty()
    visitor = Arel::MariaDBVisitor.new()
    message = nil
    begin
      visitor.quote_identifier("")
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("SQL identifier cannot be empty", message)
  end

  def test_pagination_bare_offset_uses_big_sentinel()
    visitor = Arel::MariaDBVisitor.new()
    people = Arel.table("people")
    sql, params = Arel.from(people).skip(3).to_sql(visitor)
    Minitest.assert_equal(
      "SELECT * FROM `people` LIMIT 18446744073709551615 OFFSET ?", sql)
    Minitest.assert_equal(3, params[0])
  end

  def test_pagination_limit_and_limit_with_offset()
    visitor = Arel::MariaDBVisitor.new()
    people = Arel.table("people")
    limit_only = Arel.from(people).take(5).to_sql(visitor)
    Minitest.assert_equal("SELECT * FROM `people` LIMIT ?", limit_only[0])
    Minitest.assert_equal(5, limit_only[1][0])
    both = Arel.from(people).take(5).skip(2).to_sql(visitor)
    Minitest.assert_equal("SELECT * FROM `people` LIMIT ? OFFSET ?", both[0])
    Minitest.assert_equal(5, both[1][0])
    Minitest.assert_equal(2, both[1][1])
  end

  def test_excluded_attribute_renders_as_values_function()
    visitor = Arel::MariaDBVisitor.new()
    items = Arel.table("items")
    insert = Arel.insert_into(items).values({"name": "pens", "qty": 4})
    insert = insert.on_conflict_do_update(["name"], {
      "qty": Arel.expression(Arel.excluded("qty"))
    })
    sql, params = insert.to_sql(visitor)
    Minitest.assert_equal(
      "INSERT INTO `items` (`name`, `qty`) VALUES (?, ?) " +
      "ON DUPLICATE KEY UPDATE `qty` = VALUES(`qty`)", sql)
    Minitest.assert_equal("pens", params[0])
    Minitest.assert_equal(4, params[1])
  end

  def test_on_conflict_do_nothing_renders_insert_ignore()
    visitor = Arel::MariaDBVisitor.new()
    items = Arel.table("items")
    insert = Arel.insert_into(items).values({"name": "pens"})
    insert = insert.on_conflict_do_nothing(["name"])
    sql, params = insert.to_sql(visitor)
    Minitest.assert_equal("INSERT IGNORE INTO `items` (`name`) VALUES (?)", sql)
    Minitest.assert_equal("pens", params[0])
  end

  def test_default_values_renders_empty_column_list()
    visitor = Arel::MariaDBVisitor.new()
    items = Arel.table("items")
    sql, params = Arel.insert_into(items).default_values().to_sql(visitor)
    Minitest.assert_equal("INSERT INTO `items` () VALUES ()", sql)
    Minitest.assert_equal(0, params.length())
  end

  def test_per_column_default_renders_default_keyword()
    visitor = Arel::MariaDBVisitor.new()
    items = Arel.table("items")
    insert = Arel.insert_into(items).values({"name": "pens", "qty": Arel.column_default()})
    sql, params = insert.to_sql(visitor)
    Minitest.assert_equal("INSERT INTO `items` (`name`, `qty`) VALUES (?, DEFAULT)", sql)
    Minitest.assert_equal(1, params.length())
  end

  def test_returning_on_insert_and_delete()
    visitor = Arel::MariaDBVisitor.new()
    items = Arel.table("items")
    insert_sql, insert_params = Arel.insert_into(items).values(
      {"name": "pens"}).returning([items.column("id")]).to_sql(visitor)
    Minitest.assert_equal(
      "INSERT INTO `items` (`name`) VALUES (?) RETURNING `items`.`id`", insert_sql)
    Minitest.assert_equal(1, insert_params.length())
    delete_sql, delete_params = Arel.delete_from(items).where(
      items.column("name").eq("pens")).returning([items.column("id")]).to_sql(visitor)
    Minitest.assert_equal(
      "DELETE FROM `items` WHERE `items`.`name` = ? RETURNING `items`.`id`", delete_sql)
    Minitest.assert_equal(1, delete_params.length())
  end

  def test_returning_on_update_is_rejected()
    visitor = Arel::MariaDBVisitor.new()
    items = Arel.table("items")
    update = Arel.update(items).set({"name": "pencils"}).where(
      items.column("name").eq("pens")).returning([items.column("id")])
    message = nil
    begin
      update.to_sql(visitor)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("MariaDB visitor does not support RETURNING on UPDATE", message)
  end

  def test_conflict_target_predicate_is_rejected()
    visitor = Arel::MariaDBVisitor.new()
    items = Arel.table("items")
    target = Arel.conflict_target(["name"])
    target = target.where(target.column("active").eq(Arel.literal(1)))
    insert = Arel.insert_into(items).values({"name": "pens", "active": 1})
    insert = insert.on_conflict_do_nothing(target)
    message = nil
    begin
      insert.to_sql(visitor)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("MariaDB visitor does not support conflict-target predicates", message)
  end

  def test_named_constraint_conflict_target_is_rejected()
    visitor = Arel::MariaDBVisitor.new()
    items = Arel.table("items")
    insert = Arel.insert_into(items).values({"name": "pens"})
    insert = insert.on_conflict_do_nothing(Arel.conflict_target_on_constraint("items_name_key"))
    message = nil
    begin
      insert.to_sql(visitor)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal(
      "MariaDB visitor does not support named-constraint conflict targets", message)
  end

  def test_explicit_null_ordering_is_rejected()
    visitor = Arel::MariaDBVisitor.new()
    scores = Arel.table("scores")
    ordering = Arel.asc(scores.column("score")).nulls_last()
    message = nil
    begin
      Arel.from(scores).order(ordering).to_sql(visitor)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("MariaDB visitor does not support explicit NULL ordering", message)
  end

  def test_write_ctes_are_rejected()
    visitor = Arel::MariaDBVisitor.new()
    items = Arel.table("items")
    source = Arel.from(items).project(items.column("id"))
    message = nil
    begin
      Arel.update(items).with("selected", source).set({"qty": 2}).all().to_sql(visitor)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("MariaDB visitor does not support write CTEs", message)
  end

  def test_integer_bitwise_operators_are_portable()
    visitor = Arel::MariaDBVisitor.new()
    items = Arel.table("items")
    sql, params = Arel.from(items).project(Arel.as(
      Arel.integer_operator(items.column("mask"), "&", 6), "masked")).to_sql(visitor)
    Minitest.assert_equal("SELECT (`items`.`mask` & ?) AS `masked` FROM `items`", sql)
    Minitest.assert_equal(6, params[0])
  end

  def test_ordinary_update_and_delete_render_normally()
    visitor = Arel::MariaDBVisitor.new()
    items = Arel.table("items")
    update_sql, update_params = Arel.update(items).set({"qty": 3}).where(
      items.column("name").eq("pens")).to_sql(visitor)
    Minitest.assert_equal("UPDATE `items` SET `qty` = ? WHERE `items`.`name` = ?", update_sql)
    Minitest.assert_equal(2, update_params.length())
    delete_sql, delete_params = Arel.delete_from(items).where(
      items.column("qty").eq(0)).to_sql(visitor)
    Minitest.assert_equal("DELETE FROM `items` WHERE `items`.`qty` = ?", delete_sql)
    Minitest.assert_equal(1, delete_params.length())
  end

  suite = Minitest.new()
  suite.test("backtick quoting", test_quote_identifier_uses_backticks)
  suite.test("embedded backtick escaping", test_quote_identifier_escapes_embedded_backtick)
  suite.test("empty identifier rejected", test_quote_identifier_rejects_empty)
  suite.test("bare OFFSET uses big sentinel LIMIT", test_pagination_bare_offset_uses_big_sentinel)
  suite.test("LIMIT and LIMIT+OFFSET", test_pagination_limit_and_limit_with_offset)
  suite.test("excluded attribute as VALUES()", test_excluded_attribute_renders_as_values_function)
  suite.test("ON CONFLICT DO NOTHING as INSERT IGNORE", test_on_conflict_do_nothing_renders_insert_ignore)
  suite.test("DEFAULT VALUES as empty column list", test_default_values_renders_empty_column_list)
  suite.test("per-column DEFAULT keyword", test_per_column_default_renders_default_keyword)
  suite.test("RETURNING on INSERT/DELETE", test_returning_on_insert_and_delete)
  suite.test("RETURNING on UPDATE rejected", test_returning_on_update_is_rejected)
  suite.test("conflict-target predicate rejected", test_conflict_target_predicate_is_rejected)
  suite.test("named-constraint target rejected", test_named_constraint_conflict_target_is_rejected)
  suite.test("explicit NULL ordering rejected", test_explicit_null_ordering_is_rejected)
  suite.test("write CTEs rejected", test_write_ctes_are_rejected)
  suite.test("integer bitwise operators", test_integer_bitwise_operators_are_portable)
  suite.test("ordinary UPDATE/DELETE", test_ordinary_update_and_delete_render_normally)
  suite.run!()
end

run_tests()
