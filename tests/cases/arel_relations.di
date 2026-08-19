require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_duplicate_relation_aliases_are_rejected()
    people = Arel.table("people")
    managers = people.as("managers")
    reviewers = people.as("MANAGERS")
    query = Arel.from(people).left_join(managers,
      people.column("manager_id").eq(managers.column("id")))
    message = nil
    begin
      query.left_join(reviewers, people.column("reviewer_id").eq(reviewers.column("id")))
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("duplicate relation alias in query", message)
    message = nil
    begin
      duplicate_base = people.as("PEOPLE")
      Arel.from(people).join(duplicate_base,
        people.column("id").eq(duplicate_base.column("id")))
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("duplicate relation alias in query", message)
  end

  def test_multiple_joins_preserve_sql_and_bind_order()
    people = Arel.table("people")
    companies = Arel.table("companies")
    offices = Arel.table("offices")
    query = Arel.from(people).join(companies,
      people.column("company_id").eq(companies.column("id")))
    query = query.left_join(offices, Arel.sql("\"offices\".\"id\" = ?", [7]))
    query = query.where(people.column("active").eq(true))
    sql, params = query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" INNER JOIN \"companies\" ON \"people\".\"company_id\" = \"companies\".\"id\" LEFT OUTER JOIN \"offices\" ON \"offices\".\"id\" = ? WHERE \"people\".\"active\" = ?", sql)
    Minitest.assert_equal(2, params.length())
    Minitest.assert_equal(7, params[0])
    Minitest.assert_equal(true, params[1])
  end

  suite = Minitest.new()
  suite.test("duplicate relation aliases", test_duplicate_relation_aliases_are_rejected)
  suite.test("multiple join and bind order", test_multiple_joins_preserve_sql_and_bind_order)
  suite.run!()
end

run_tests()
