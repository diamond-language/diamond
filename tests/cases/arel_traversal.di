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

  def test_predicate_children_preserve_semantic_order()
    people = Arel.table("people")
    roles = Arel.table("roles")
    predicate = people.column("role_id").eq(roles.column("id"))
    children = Arel.children(predicate)
    Minitest.assert_equal("Attribute(people.role_id)", Arel.inspect(children[0]))
    Minitest.assert_equal("Attribute(roles.id)", Arel.inspect(children[1]))
    logical = predicate.and_also(people.column("active").eq(true))
    Minitest.assert_equal(2, Arel.children(logical).length())
    Minitest.assert_equal(1, Arel.children(people.column("age").between(18, 65)).length())
    subquery = Arel.from(roles).project(roles.column("id"))
    Minitest.assert_equal(2, Arel.children(people.column("role_id").in_subquery(subquery)).length())
  end

  suite = Minitest.new()
  suite.test("ordered expression children", test_expression_children_are_ordered)
  suite.test("ordered predicate children", test_predicate_children_preserve_semantic_order)
  suite.run()
end

run_tests()
