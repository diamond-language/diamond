require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_hash_where_renders_equality()
    query = Arel.from("people").where({"active": 1})
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM people WHERE active = ?", sql)
    Minitest.assert_equal(1, params.length())
    Minitest.assert_equal(1, params[0])
  end

  def test_raw_fragment_where_with_params()
    query = Arel.from("people").where("age >= ?", [18])
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM people WHERE age >= ?", sql)
    Minitest.assert_equal(1, params.length())
    Minitest.assert_equal(18, params[0])
  end

  def test_multiple_wheres_and_together()
    query = Arel.from("people").where({"active": 1}).where("age >= ?", [18])
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM people WHERE active = ? AND age >= ?", sql)
    Minitest.assert_equal(2, params.length())
    Minitest.assert_equal(1, params[0])
    Minitest.assert_equal(18, params[1])
  end

  def test_select_order_limit_offset()
    query = Arel.from("people").select(["name", "age"]).order("age DESC").limit(2).offset(1)
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT name, age FROM people ORDER BY age DESC LIMIT 2 OFFSET 1", sql)
    Minitest.assert_equal(0, params.length())
  end

  def test_order_accepts_an_array_of_columns()
    query = Arel.from("people").order(["name", "age DESC"])
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM people ORDER BY name, age DESC", sql)
  end

  def test_base_query_is_not_mutated_by_branches()
    base = Arel.from("people").where({"active": 1})
    adults = base.where("age >= ?", [18])
    minors = base.where("age < ?", [18])

    base_sql, base_params = base.to_sql()
    adults_sql, adults_params = adults.to_sql()
    minors_sql, minors_params = minors.to_sql()

    Minitest.assert_equal("SELECT * FROM people WHERE active = ?", base_sql)
    Minitest.assert_equal("SELECT * FROM people WHERE active = ? AND age >= ?", adults_sql)
    Minitest.assert_equal("SELECT * FROM people WHERE active = ? AND age < ?", minors_sql)
  end

  def test_to_a_runs_the_query_against_a_real_db()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, age INTEGER, active INTEGER)")
    db.execute("INSERT INTO people (name, age, active) VALUES (?, ?, ?)", ["Ada", 30, 1])
    db.execute("INSERT INTO people (name, age, active) VALUES (?, ?, ?)", ["Bob", 15, 1])
    db.execute("INSERT INTO people (name, age, active) VALUES (?, ?, ?)", ["Cid", 40, 0])

    adults = Arel.from("people").where({"active": 1}).where("age >= ?", [18])
    rows = adults.to_a(db)
    Minitest.assert_equal(1, rows.length())
    Minitest.assert_equal("Ada", rows[0]["name"])

    db.close()
  end

  def test_count_wraps_the_full_query()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, age INTEGER, active INTEGER)")
    db.execute("INSERT INTO people (name, age, active) VALUES (?, ?, ?)", ["Ada", 30, 1])
    db.execute("INSERT INTO people (name, age, active) VALUES (?, ?, ?)", ["Bob", 15, 1])
    db.execute("INSERT INTO people (name, age, active) VALUES (?, ?, ?)", ["Cid", 40, 0])

    active = Arel.from("people").where({"active": 1})
    Minitest.assert_equal(2, active.count(db))

    db.close()
  end

  suite = Minitest.new()
  suite.test("hash where renders equality", test_hash_where_renders_equality)
  suite.test("raw fragment where with params", test_raw_fragment_where_with_params)
  suite.test("multiple wheres AND together", test_multiple_wheres_and_together)
  suite.test("select/order/limit/offset", test_select_order_limit_offset)
  suite.test("order accepts an array of columns", test_order_accepts_an_array_of_columns)
  suite.test("base query is not mutated by branches", test_base_query_is_not_mutated_by_branches)
  suite.test("to_a runs the query against a real db", test_to_a_runs_the_query_against_a_real_db)
  suite.test("count wraps the full query", test_count_wraps_the_full_query)
  suite.run()
end

run_tests()
