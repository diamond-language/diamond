require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_addition_is_a_structural_update_expression()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE counters (id INTEGER, value INTEGER)")
    db.execute("INSERT INTO counters VALUES (1, 4)")
    counters = Arel.table("counters")
    update = Arel.update(counters).set({
      "value": Arel.expression(counters.column("value").add(3))
    }).where(counters.column("id").eq(1))
    sql, params = update.to_sql()
    Minitest.assert_equal("UPDATE \"counters\" SET \"value\" = (\"counters\".\"value\" + ?) WHERE \"counters\".\"id\" = ?", sql)
    Minitest.assert_equal(3, params[0])
    Minitest.assert_equal(1, params[1])
    update.execute(db)
    Minitest.assert_equal(7, db.query("SELECT value FROM counters")[0]["value"])
    db.close()
  end

  def test_subtraction_is_structural_and_chainable()
    balances = Arel.table("balances")
    expression = balances.column("amount").subtract(4).add(1)
    update = Arel.update(balances).set({"amount": Arel.expression(expression)}).all()
    sql, params = update.to_sql()
    Minitest.assert_equal("UPDATE \"balances\" SET \"amount\" = ((\"balances\".\"amount\" - ?) + ?)", sql)
    Minitest.assert_equal(4, params[0])
    Minitest.assert_equal(1, params[1])
  end

  def test_multiplication_preserves_grouping()
    prices = Arel.table("prices")
    expression = prices.column("amount").add(2).multiply(3)
    update = Arel.update(prices).set({"amount": Arel.expression(expression)}).all()
    sql, params = update.to_sql()
    Minitest.assert_equal("UPDATE \"prices\" SET \"amount\" = ((\"prices\".\"amount\" + ?) * ?)", sql)
    Minitest.assert_equal(2, params[0])
    Minitest.assert_equal(3, params[1])
  end

  def test_division_renders_as_a_bound_expression()
    measurements = Arel.table("measurements")
    expression = measurements.column("total").divide(2)
    sql, params = Arel.update(measurements).set({
      "total": Arel.expression(expression)
    }).all().to_sql()
    Minitest.assert_equal("UPDATE \"measurements\" SET \"total\" = (\"measurements\".\"total\" / ?)", sql)
    Minitest.assert_equal(2, params[0])
  end

  def test_excluded_attributes_support_arithmetic()
    inventory = Arel.table("inventory")
    insert = Arel.insert_into(inventory).values({"name": "pens", "qty": 4})
    insert = insert.on_conflict_do_update(["name"], {
      "qty": Arel.expression(Arel.excluded("qty").add(1))
    })
    sql, params = insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"inventory\" (\"name\", \"qty\") VALUES (?, ?) ON CONFLICT (\"name\") DO UPDATE SET \"qty\" = (excluded.\"qty\" + ?)", sql)
    Minitest.assert_equal(1, params[2])
  end

  def test_arithmetic_accepts_structural_right_operands()
    totals = Arel.table("totals")
    expression = totals.column("subtotal").add_expression(totals.column("tax"))
    update = Arel.update(totals).set({"grand_total": Arel.expression(expression)}).all()
    sql, params = update.to_sql()
    Minitest.assert_equal("UPDATE \"totals\" SET \"grand_total\" = (\"totals\".\"subtotal\" + \"totals\".\"tax\")", sql)
    Minitest.assert_equal(0, params.length())
  end

  def test_restricted_literals_render_without_binds()
    flags = Arel.table("flags")
    query = Arel.from(flags).where(flags.column("enabled").eq(Arel.literal(true)))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"flags\" WHERE \"flags\".\"enabled\" = TRUE", sql)
    Minitest.assert_equal(0, params.length())
  end

  def test_structural_literals_reject_strings()
    message = nil
    begin
      Arel.literal("unsafe")
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("SQL literals only support Int and Bool values", message)
  end

  suite = Minitest.new()
  suite.test("structural addition", test_addition_is_a_structural_update_expression)
  suite.test("structural subtraction", test_subtraction_is_structural_and_chainable)
  suite.test("structural multiplication", test_multiplication_preserves_grouping)
  suite.test("structural division", test_division_renders_as_a_bound_expression)
  suite.test("excluded arithmetic", test_excluded_attributes_support_arithmetic)
  suite.test("structural arithmetic operands", test_arithmetic_accepts_structural_right_operands)
  suite.test("restricted SQL literals", test_restricted_literals_render_without_binds)
  suite.test("SQL literal validation", test_structural_literals_reject_strings)
  suite.run!()
end

run_tests()
