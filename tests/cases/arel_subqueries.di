require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_subquery_can_be_used_as_from_source()
    people = Arel.table("people")
    inner = Arel.from(people).project(people.column("name"))
    inner = inner.where(people.column("active").eq(true))
    derived = Arel.table("active_people")
    outer = Arel.from_subquery(inner, "active_people")
    outer = outer.project(derived.column("name"))
    sql, params = outer.to_sql()
    Minitest.assert_equal("SELECT \"active_people\".\"name\" FROM (SELECT \"people\".\"name\" FROM \"people\" WHERE \"people\".\"active\" = ?) AS \"active_people\"", sql)
    Minitest.assert_equal(true, params[0])
  end

  def test_exists_and_not_exists_are_predicates()
    people = Arel.table("people")
    inner = Arel.from(people).where(people.column("active").eq(true))
    outer_table = Arel.table("settings")
    predicate = Arel.exists(inner).and_also(Arel.not_exists(inner).not_())
    sql, params = Arel.from(outer_table).where(predicate).to_sql()
    Minitest.assert_equal("SELECT * FROM \"settings\" WHERE (EXISTS (SELECT * FROM \"people\" WHERE \"people\".\"active\" = ?) AND EXISTS (SELECT * FROM \"people\" WHERE \"people\".\"active\" = ?))", sql)
    Minitest.assert_equal(2, params.length())
    Minitest.assert_equal(true, params[0])
  end

  def test_in_subquery_preserves_inner_binds()
    memberships = Arel.table("memberships")
    inner = Arel.from(memberships).project(memberships.column("person_id"))
    inner = inner.where(memberships.column("role").eq("admin"))
    people = Arel.table("people")
    query = Arel.from(people).where(people.column("id").in_subquery(inner))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE \"people\".\"id\" IN (SELECT \"memberships\".\"person_id\" FROM \"memberships\" WHERE \"memberships\".\"role\" = ?)", sql)
    Minitest.assert_equal("admin", params[0])
  end

  def test_scalar_subquery_is_an_expression()
    scores = Arel.table("scores")
    inner = Arel.from(scores).project(Arel.max(scores.column("value")))
    inner = inner.where(scores.column("active").eq(true))
    people = Arel.table("people")
    maximum = Arel.as(Arel.scalar(inner), "maximum_score")
    sql, params = Arel.from(people).project([people.column("name"), maximum]).to_sql()
    Minitest.assert_equal("SELECT \"people\".\"name\", (SELECT MAX(\"scores\".\"value\") FROM \"scores\" WHERE \"scores\".\"active\" = ?) AS \"maximum_score\" FROM \"people\"", sql)
    Minitest.assert_equal(true, params[0])
  end

  def test_explicit_correlation_allows_outer_attributes()
    people = Arel.table("people")
    memberships = Arel.table("memberships")
    inner = Arel.from(memberships).correlate(people)
    inner = inner.where(memberships.column("person_id").eq(people.column("id")))
    sql, params = Arel.from(people).where(Arel.exists(inner)).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE EXISTS (SELECT * FROM \"memberships\" WHERE \"memberships\".\"person_id\" = \"people\".\"id\")", sql)
  end

  def test_correlation_rejects_local_and_duplicate_relations()
    people = Arel.table("people")
    memberships = Arel.table("memberships")
    local_message = nil
    duplicate_message = nil
    begin
      Arel.from(memberships).correlate(memberships)
    rescue error: ArgumentError
      local_message = error.message()
    end
    begin
      Arel.from(memberships).correlate(people).correlate(people)
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
  suite.test("correlation validation", test_correlation_rejects_local_and_duplicate_relations)
  suite.test("nested correlation", test_nested_correlation_can_name_each_outer_level)
  suite.run()
end

run_tests()
