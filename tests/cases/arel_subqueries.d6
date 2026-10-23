require "../../lib/minitest"
require "../../packages/arel/lib/arel"

def run_tests()
  def test_subquery_can_be_used_as_from_source()
    people = Arel.table("people")
    inner = Arel.from(people).project(people.column("name"))
    inner = inner.where(people.column("active").eq(true))
    inner = inner.order(people.column("name").asc()).take(2)
    derived = Arel.table("active_people")
    payload = "Ada'); DROP TABLE people; --"
    outer = Arel.from_subquery(inner, "active_people")
    outer = outer.project(derived.column("name")).where(derived.column("name").eq(payload))
    sql, params = outer.to_sql()
    Minitest.assert_equal("SELECT \"active_people\".\"name\" FROM (SELECT \"people\".\"name\" FROM \"people\" WHERE \"people\".\"active\" = ? ORDER BY \"people\".\"name\" ASC LIMIT ?) AS \"active_people\" WHERE \"active_people\".\"name\" = ?", sql)
    Minitest.assert_equal(true, params[0])
    Minitest.assert_equal("2|#{payload}", [params[1], params[2]].join("|"))
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE people (name TEXT, active INTEGER)")
    db.execute("INSERT INTO people VALUES (?, ?)", [payload, 1])
    db.execute("INSERT INTO people VALUES ('Bob', 1), ('Cid', 0)")
    rows = outer.to_a(db)
    Minitest.assert_equal(1, rows.length())
    Minitest.assert_equal(payload, rows[0]["name"])
    Minitest.assert_equal(3, db.query("SELECT * FROM people").length())
    db.close()
  end

  def test_exists_and_not_exists_are_predicates()
    people = Arel.table("people")
    inner = Arel.from(people).where(people.column("active").eq(true))
    inner = inner.take(2)
    outer_table = Arel.table("settings")
    predicate = Arel.exists(inner).and_also(Arel.not_exists(inner).not_())
    sql, params = Arel.from(outer_table).where(predicate).to_sql()
    Minitest.assert_equal("SELECT * FROM \"settings\" WHERE (EXISTS (SELECT * FROM \"people\" WHERE \"people\".\"active\" = ? LIMIT ?) AND EXISTS (SELECT * FROM \"people\" WHERE \"people\".\"active\" = ? LIMIT ?))", sql)
    Minitest.assert_equal("true|2|true|2", params.join("|"))
  end

  def test_in_subquery_preserves_inner_binds()
    memberships = Arel.table("memberships")
    inner = Arel.from(memberships).project(memberships.column("person_id"))
    inner = inner.where(memberships.column("role").eq("admin"))
    inner = inner.order(memberships.column("person_id").asc()).take(4)
    people = Arel.table("people")
    query = Arel.from(people).where(people.column("id").in_subquery(inner))
    query = query.where(people.column("active").eq(true))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE \"people\".\"id\" IN (SELECT \"memberships\".\"person_id\" FROM \"memberships\" WHERE \"memberships\".\"role\" = ? ORDER BY \"memberships\".\"person_id\" ASC LIMIT ?) AND \"people\".\"active\" = ?", sql)
    Minitest.assert_equal("admin|4|true", params.join("|"))
  end

  def test_scalar_subquery_is_an_expression()
    scores = Arel.table("scores")
    inner = Arel.from(scores).project(Arel.max(scores.column("value")))
    inner = inner.where(scores.column("active").eq(true))
    inner = inner.take(1)
    people = Arel.table("people")
    maximum = Arel.as(Arel.scalar(inner), "maximum_score")
    query = Arel.from(people).project([people.column("name"), maximum])
    query = query.where(people.column("name").eq("Ada"))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT \"people\".\"name\", (SELECT MAX(\"scores\".\"value\") FROM \"scores\" WHERE \"scores\".\"active\" = ? LIMIT ?) AS \"maximum_score\" FROM \"people\" WHERE \"people\".\"name\" = ?", sql)
    Minitest.assert_equal("true|1|Ada", params.join("|"))
  end

  def test_explicit_correlation_allows_outer_attributes()
    people = Arel.table("people")
    memberships = Arel.table("memberships")
    inner = Arel.from(memberships).correlate(people)
    inner = inner.where(memberships.column("person_id").eq(people.column("id")))
    sql, params = Arel.from(people).where(Arel.exists(inner)).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE EXISTS (SELECT * FROM \"memberships\" WHERE \"memberships\".\"person_id\" = \"people\".\"id\")", sql)
  end

  def test_correlated_exists_executes_against_sqlite()
    people = Arel.table("people")
    memberships = Arel.table("memberships")
    inner = Arel.from(memberships).correlate(people)
    inner = inner.where(memberships.column("person_id").eq(people.column("id")))
    query = Arel.from(people).where(Arel.exists(inner))

    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE people (id INTEGER, name TEXT)")
    db.execute("CREATE TABLE memberships (person_id INTEGER)")
    db.execute("INSERT INTO people VALUES (1, 'Ada'), (2, 'Bob')")
    db.execute("INSERT INTO memberships VALUES (1)")

    rows = query.to_a(db)
    Minitest.assert_equal(1, rows.length())
    Minitest.assert_equal("Ada", rows[0]["name"])
    db.close()
  end

  def test_correlation_rejects_local_and_duplicate_relations()
    people = Arel.table("people")
    memberships = Arel.table("memberships")
    local_message = nil
    duplicate_message = nil
    begin
      Arel.from(memberships).correlate(memberships.as("MEMBERSHIPS"))
    rescue error: ArgumentError
      local_message = error.message()
    end
    begin
      Arel.from(memberships).correlate(people).correlate(people.as("PEOPLE"))
    rescue error: ArgumentError
      duplicate_message = error.message()
    end
    Minitest.assert_equal("correlation must reference an outer relation", local_message)
    Minitest.assert_equal("duplicate correlated relation", duplicate_message)
  end

  def test_nested_correlation_can_name_each_outer_level()
    companies = Arel.table("companies")
    people = Arel.table("people")
    memberships = Arel.table("memberships")
    inner = Arel.from(memberships).correlate_all([people, companies])
    person_match = memberships.column("person_id").eq(people.column("id"))
    inner_predicate = person_match.and_also(
      memberships.column("company_id").eq(companies.column("id")))
    inner = inner.where(inner_predicate)
    middle = Arel.from(people).correlate(companies).where(Arel.exists(inner))
    outer = Arel.from(companies).where(Arel.exists(middle))
    sql, params = outer.to_sql()
    Minitest.assert_equal("SELECT * FROM \"companies\" WHERE EXISTS (SELECT * FROM \"people\" WHERE EXISTS (SELECT * FROM \"memberships\" WHERE (\"memberships\".\"person_id\" = \"people\".\"id\" AND \"memberships\".\"company_id\" = \"companies\".\"id\")))", sql)
  end

  suite = Minitest.new()
  suite.test("FROM subquery", test_subquery_can_be_used_as_from_source)
  suite.test("EXISTS predicates", test_exists_and_not_exists_are_predicates)
  suite.test("IN subquery", test_in_subquery_preserves_inner_binds)
  suite.test("scalar subquery", test_scalar_subquery_is_an_expression)
  suite.test("explicit correlation", test_explicit_correlation_allows_outer_attributes)
  suite.test("correlated execution", test_correlated_exists_executes_against_sqlite)
  suite.test("correlation validation", test_correlation_rejects_local_and_duplicate_relations)
  suite.test("nested correlation", test_nested_correlation_can_name_each_outer_level)
  suite.run!()
end

run_tests()
