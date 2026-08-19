require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_duplicate_relation_aliases_are_rejected()
    people = Arel.table("people")
    managers = people.as("managers")
    reviewers = people.as("managers")
    query = Arel.from(people).left_join(managers,
      people.column("manager_id").eq(managers.column("id")))
    message = nil
    begin
      query.left_join(reviewers, people.column("reviewer_id").eq(reviewers.column("id")))
    rescue error: ArgumentError
      message = error.message()
    end
    Minitest.assert_equal("duplicate relation alias in query", message)
  end

  suite = Minitest.new()
  suite.test("duplicate relation aliases", test_duplicate_relation_aliases_are_rejected)
  suite.run()
end

run_tests()
