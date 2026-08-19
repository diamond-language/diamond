require "../../lib/minitest"
require "../../packages/arel/arel"

def run_tests()
  def test_expression_children_are_ordered()
    people = Arel.table("people")
    expression = people.column("score").add_expression(Arel.literal(2))
    children = Arel.children(expression)
    Minitest.assert_equal(2, children.length())
    Minitest.assert_equal("Attribute(people.score)", Arel.inspect(children[0]))
    Minitest.assert_equal("Literal(2)", Arel.inspect(children[1]))
    Minitest.assert_equal(1, Arel.children(people.column("score").add(2)).length())
    function_children = Arel.children(Arel.function("COALESCE", [people.column("name"), Arel.literal(0)]))
    Minitest.assert_equal("Attribute(people.name)", Arel.inspect(function_children[0]))
    Minitest.assert_equal("Literal(0)", Arel.inspect(function_children[1]))
  end

  suite = Minitest.new()
  suite.test("ordered expression children", test_expression_children_are_ordered)
  suite.run()
end

run_tests()
