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

  suite = Minitest.new()
  suite.test("structural addition", test_addition_is_a_structural_update_expression)
  suite.test("structural subtraction", test_subtraction_is_structural_and_chainable)
  suite.test("structural multiplication", test_multiplication_preserves_grouping)
  suite.run()
end

run_tests()
