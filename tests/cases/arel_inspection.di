require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_core_expressions_have_deterministic_inspection()
    people = Arel.table("people")
    arithmetic = people.column("score").add_expression(Arel.literal(2))
    cast = Arel.cast(arithmetic, "TEXT")
    function = Arel.function("LOWER", [cast])
    Minitest.assert_equal("Attribute(people.score)", Arel.inspect(people.column("score")))
    Minitest.assert_equal("Binary(+, Attribute(people.score), Literal(2))", Arel.inspect(arithmetic))
    Minitest.assert_equal("Function(LOWER, [Cast(Binary(+, Attribute(people.score), Literal(2)), TEXT)])", Arel.inspect(function))
    Minitest.assert_equal("Excluded(qty)", Arel.inspect(Arel.excluded("qty")))
  end

  suite = Minitest.new()
  suite.test("core expression inspection", test_core_expressions_have_deterministic_inspection)
  suite.run()
end

run_tests()
