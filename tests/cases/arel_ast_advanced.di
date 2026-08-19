require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_between_predicates_bind_both_bounds()
    people = Arel.table("people")
    range = people.column("age").between(18, 65)
    predicate = range.and_also(people.column("score").not_between(0, 10))
    sql, params = Arel.from(people).where(predicate).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE (\"people\".\"age\" BETWEEN ? AND ? AND \"people\".\"score\" NOT BETWEEN ? AND ?)", sql)
    Minitest.assert_equal(4, params.length())
    Minitest.assert_equal(18, params[0])
    Minitest.assert_equal(10, params[3])
  end

  def test_like_predicates_are_bound()
    people = Arel.table("people")
    payload = "x'); DROP TABLE people; --"
    starts_with_a = people.column("name").like(payload)
    predicate = starts_with_a.and_also(people.column("email").not_like("%@spam.test"))
    sql, params = Arel.from(people).where(predicate).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE (\"people\".\"name\" LIKE ? AND \"people\".\"email\" NOT LIKE ?)", sql)
    Minitest.assert_equal(payload, params[0])
    Minitest.assert_equal("%@spam.test", params[1])
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE people (name TEXT, email TEXT)")
    db.execute("INSERT INTO people VALUES (?, ?)", [payload, "safe@example.test"])
    Minitest.assert_equal(1, Arel.from(people).where(predicate).to_a(db).length())
    Minitest.assert_equal(1, db.query("SELECT * FROM people").length())
    db.close()
  end

  def test_function_and_aggregate_projections()
    people = Arel.table("people")
    query = Arel.from(people).project([
      Arel.count(people.column("id")),
      Arel.avg(people.column("age")),
      Arel.upper(people.column("name"))
    ])
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT COUNT(\"people\".\"id\"), AVG(\"people\".\"age\"), UPPER(\"people\".\"name\") FROM \"people\"", sql)
    Minitest.assert_empty(params)
  end

  def test_group_by_accepts_expression_arrays()
    people = Arel.table("people")
    query = Arel.from(people).project([people.column("role"), Arel.count(people.column("id"))])
    sql, params = query.group(people.column("role")).to_sql()
    Minitest.assert_equal("SELECT \"people\".\"role\", COUNT(\"people\".\"id\") FROM \"people\" GROUP BY \"people\".\"role\"", sql)
  end

  def test_having_uses_expression_nodes_and_binds()
    people = Arel.table("people")
    count = Arel.count(people.column("id"))
    query = Arel.from(people).project([people.column("role"), count])
    sql, params = query.group(people.column("role")).having(count.gt(2)).to_sql()
    Minitest.assert_equal("SELECT \"people\".\"role\", COUNT(\"people\".\"id\") FROM \"people\" GROUP BY \"people\".\"role\" HAVING COUNT(\"people\".\"id\") > ?", sql)
    Minitest.assert_equal(2, params[0])
  end

  def test_explicit_sql_literal_is_composable()
    people = Arel.table("people")
    payload = "x'); DROP TABLE people; --"
    query = Arel.from(people).project(people.column("profile"))
    query = query.where(Arel.sql("profile = ?", [payload]))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT \"people\".\"profile\" FROM \"people\" WHERE profile = ?", sql)
    Minitest.assert_equal(payload, params[0])
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE people (profile TEXT)")
    db.execute("INSERT INTO people VALUES (?)", [payload])
    Minitest.assert_equal(1, query.to_a(db).length())
    Minitest.assert_equal(1, db.query("SELECT * FROM people").length())
    db.close()
  end

  def test_inner_join_is_structural_and_qualified()
    people = Arel.table("people")
    companies = Arel.table("companies")
    on = people.column("company_id").eq(companies.column("id"))
    query = Arel.from(people).join(companies, on)
    sql, params = query.project([people.column("name"), companies.column("name")]).to_sql()
    Minitest.assert_equal("SELECT \"people\".\"name\", \"companies\".\"name\" FROM \"people\" INNER JOIN \"companies\" ON \"people\".\"company_id\" = \"companies\".\"id\"", sql)
    Minitest.assert_empty(params)
  end

  def test_left_outer_join_executes_against_sqlite()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE people (name TEXT, company_id INTEGER)")
    db.execute("CREATE TABLE companies (id INTEGER, name TEXT)")
    db.execute("INSERT INTO people VALUES (?, ?)", ["Ada", nil])
    people = Arel.table("people")
    companies = Arel.table("companies")
    on = people.column("company_id").eq(companies.column("id"))
    query = Arel.from(people).left_join(companies, on)
    rows = query.project([people.column("name"), companies.column("name").as("company")]).to_a(db)
    Minitest.assert_equal(1, rows.length())
    Minitest.assert_equal("Ada", rows[0]["name"])
    Minitest.assert_nil(rows[0]["company"])
    db.close()
  end

  def test_self_join_uses_distinct_relation_aliases()
    people = Arel.table("people")
    managers = people.as("managers")
    on = people.column("manager_id").eq(managers.column("id"))
    query = Arel.from(people).left_join(managers, on)
    sql, params = query.project([people.column("name"), managers.column("name").as("manager")]).to_sql()
    Minitest.assert_equal("SELECT \"people\".\"name\", \"managers\".\"name\" AS \"manager\" FROM \"people\" LEFT OUTER JOIN \"people\" AS \"managers\" ON \"people\".\"manager_id\" = \"managers\".\"id\"", sql)
  end

  def test_attributes_outside_relation_set_are_rejected()
    people = Arel.table("people")
    accounts = Arel.table("accounts")
    message = nil
    begin
      Arel.from(people).project(accounts.column("id")).to_sql()
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("attribute belongs to a relation outside this query", message)
    query = Arel.from(Arel.table("People")).project(Arel.table("people").column("id"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT \"people\".\"id\" FROM \"People\"", sql)
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE \"People\" (id INTEGER)")
    db.execute("INSERT INTO \"People\" VALUES (7)")
    Minitest.assert_equal(7, query.to_a(db)[0]["id"])
    db.close()
  end

  def test_arbitrary_expressions_can_be_aliased()
    people = Arel.table("people")
    average = Arel.as(Arel.avg(people.column("age")), "average_age")
    sql, params = Arel.from(people).project(average).to_sql()
    Minitest.assert_equal("SELECT AVG(\"people\".\"age\") AS \"average_age\" FROM \"people\"", sql)
  end

  def test_arbitrary_expression_ordering_and_null_placement()
    people = Arel.table("people")
    ordering = Arel.desc(Arel.lower(people.column("name"))).nulls_last()
    sql, params = Arel.from(people).order(ordering).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" ORDER BY LOWER(\"people\".\"name\") DESC NULLS LAST", sql)
  end

  def test_qualified_wildcard_projection()
    people = Arel.table("people").as("p")
    sql, params = Arel.from(people).project(people.star()).to_sql()
    Minitest.assert_equal("SELECT \"p\".* FROM \"people\" AS \"p\"", sql)
  end

  def test_distinct_function_arguments()
    people = Arel.table("people")
    count = Arel.as(Arel.count_distinct(people.column("role")), "roles")
    sql, params = Arel.from(people).project(count).to_sql()
    Minitest.assert_equal("SELECT COUNT(DISTINCT \"people\".\"role\") AS \"roles\" FROM \"people\"", sql)
  end

  def test_sqlite_collation_expression()
    people = Arel.table("people")
    ordering = Arel.asc(people.column("name").collate("NOCASE"))
    sql, params = Arel.from(people).order(ordering).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" ORDER BY \"people\".\"name\" COLLATE \"NOCASE\" ASC", sql)
  end

  def test_cross_join_has_no_on_clause()
    colors = Arel.table("colors")
    sizes = Arel.table("sizes")
    query = Arel.from(colors).cross_join(sizes)
    sql, params = query.project([colors.column("name"), sizes.column("label")]).to_sql()
    Minitest.assert_equal("SELECT \"colors\".\"name\", \"sizes\".\"label\" FROM \"colors\" CROSS JOIN \"sizes\"", sql)
  end

  suite = Minitest.new()
  suite.test("BETWEEN predicates", test_between_predicates_bind_both_bounds)
  suite.test("LIKE predicates", test_like_predicates_are_bound)
  suite.test("function and aggregate projections", test_function_and_aggregate_projections)
  suite.test("GROUP BY", test_group_by_accepts_expression_arrays)
  suite.test("HAVING", test_having_uses_expression_nodes_and_binds)
  suite.test("explicit SQL literal", test_explicit_sql_literal_is_composable)
  suite.test("inner join", test_inner_join_is_structural_and_qualified)
  suite.test("left outer join", test_left_outer_join_executes_against_sqlite)
  suite.test("self join aliases", test_self_join_uses_distinct_relation_aliases)
  suite.test("relation-set validation", test_attributes_outside_relation_set_are_rejected)
  suite.test("arbitrary expression aliases", test_arbitrary_expressions_can_be_aliased)
  suite.test("expression ordering and null placement", test_arbitrary_expression_ordering_and_null_placement)
  suite.test("qualified wildcard", test_qualified_wildcard_projection)
  suite.test("distinct function arguments", test_distinct_function_arguments)
  suite.test("SQLite collation", test_sqlite_collation_expression)
  suite.test("cross join", test_cross_join_has_no_on_clause)
  suite.run!()
end

run_tests()
