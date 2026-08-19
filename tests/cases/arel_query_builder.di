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
    query = Arel.from(Arel.table("people")).skip(3)
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" LIMIT -1 OFFSET ?", sql)
    Minitest.assert_equal("3", params.join("|"))
    message = nil
    begin
      Arel.from("people").limit(-1)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("limit must be non-negative", message)
    message = nil
    begin
      Arel.from("people").offset(-1)
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("offset must be non-negative", message)
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

    people = Arel.table("people")
    rows = Arel.from(people).order(people.column("id").asc()).skip(1).to_a(db)
    Minitest.assert_equal(2, rows.length())
    Minitest.assert_equal("Bob", rows[0]["name"])

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

  def test_ast_query_quotes_identifiers_and_binds_values()
    people = Arel.table("people")
    query = Arel.from(people).project([people.column("name"), people.column("age")])
    query = query.where(people.column("active").eq(true).and_also(people.column("age").gteq(18)))
    query = query.order(people.column("name").asc()).take(20).skip(5)
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT \"people\".\"name\", \"people\".\"age\" FROM \"people\" WHERE (\"people\".\"active\" = ? AND \"people\".\"age\" >= ?) ORDER BY \"people\".\"name\" ASC LIMIT ? OFFSET ?", sql)
    Minitest.assert_equal(4, params.length())
    Minitest.assert_equal(true, params[0])
    Minitest.assert_equal(18, params[1])
    Minitest.assert_equal(20, params[2])
    Minitest.assert_equal(5, params[3])
  end

  def test_or_not_and_null_predicates()
    people = Arel.table("people")
    roles = people.column("role").eq("admin").or_else(people.column("role").eq("owner")).not_()
    predicate = people.column("deleted_at").eq(nil).and_also(roles)
    sql, params = Arel.from(people).where(predicate).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE (\"people\".\"deleted_at\" IS NULL AND (NOT (\"people\".\"role\" = ? OR \"people\".\"role\" = ?)))", sql)
    Minitest.assert_equal(2, params.length())
    Minitest.assert_equal("admin", params[0])
    Minitest.assert_equal("owner", params[1])
  end

  def test_not_eq_nil_uses_is_not_null()
    people = Arel.table("people")
    sql, params = Arel.from(people).where(people.column("name").not_eq(nil)).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE \"people\".\"name\" IS NOT NULL", sql)
    Minitest.assert_empty(params)
  end

  def test_identifier_quotes_are_escaped()
    unusual = Arel.table("user\"data")
    sql, params = Arel.from(unusual).project(unusual.column("say\"hi")).to_sql()
    Minitest.assert_equal("SELECT \"user\"\"data\".\"say\"\"hi\" FROM \"user\"\"data\"", sql)
  end

  def test_ast_executes_against_sqlite()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE people (name TEXT, age INTEGER, active INTEGER)")
    db.execute("INSERT INTO people VALUES (?, ?, ?)", ["Ada", 30, 1])
    db.execute("INSERT INTO people VALUES (?, ?, ?)", ["Bob", 15, 1])
    people = Arel.table("people")
    query = Arel.from(people).project(people.column("name"))
    rows = query.where(people.column("age").gteq(18)).to_a(db)
    Minitest.assert_equal(1, rows.length())
    Minitest.assert_equal("Ada", rows[0]["name"])
    db.close()
  end

  def test_table_and_projection_aliases()
    people = Arel.table("people").as("p")
    query = Arel.from(people).project(people.column("name").as("display_name"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT \"p\".\"name\" AS \"display_name\" FROM \"people\" AS \"p\"", sql)
  end

  def test_distinct_projection()
    people = Arel.table("people")
    sql, params = Arel.from(people).project(people.column("role")).distinct().to_sql()
    Minitest.assert_equal("SELECT DISTINCT \"people\".\"role\" FROM \"people\"", sql)
  end

  def test_in_and_not_in_predicates()
    people = Arel.table("people")
    included = people.column("role").in_list(["admin", "owner"])
    predicate = included.and_also(people.column("id").not_in([4, 9]))
    sql, params = Arel.from(people).where(predicate).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE (\"people\".\"role\" IN (?, ?) AND \"people\".\"id\" NOT IN (?, ?))", sql)
    Minitest.assert_equal(4, params.length())
    Minitest.assert_equal("admin", params[0])
    Minitest.assert_equal(9, params[3])
  end

  def test_empty_in_lists_are_valid_boolean_expressions()
    people = Arel.table("people")
    empty_sql, empty_params = Arel.from(people).where(people.column("id").in_list([])).to_sql()
    not_empty_sql, not_empty_params = Arel.from(people).where(people.column("id").not_in([])).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE 1 = 0", empty_sql)
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE 1 = 1", not_empty_sql)
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
  suite.test("AST query quotes identifiers and binds values", test_ast_query_quotes_identifiers_and_binds_values)
  suite.test("OR, NOT, and NULL predicates", test_or_not_and_null_predicates)
  suite.test("not-equal nil uses IS NOT NULL", test_not_eq_nil_uses_is_not_null)
  suite.test("identifier quotes are escaped", test_identifier_quotes_are_escaped)
  suite.test("AST executes against SQLite", test_ast_executes_against_sqlite)
  suite.test("table and projection aliases", test_table_and_projection_aliases)
  suite.test("distinct projection", test_distinct_projection)
  suite.test("IN and NOT IN predicates", test_in_and_not_in_predicates)
  suite.test("empty IN lists", test_empty_in_lists_are_valid_boolean_expressions)
  suite.run!()
end

run_tests()
