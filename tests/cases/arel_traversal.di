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

  def test_query_children_follow_render_order()
    people = Arel.table("people")
    roles = Arel.table("roles")
    cte_body = Arel.from(roles).project(roles.column("id"))
    query = Arel.from(people).with("role_ids", cte_body)
    query = query.project(people.column("name")).join(roles,
      people.column("role_id").eq(roles.column("id")))
    query = query.where(people.column("active").eq(true)).order(people.column("name").asc())
    children = Arel.children(query)
    Minitest.assert_equal("Cte(role_ids, ordinary, Query(from=roles, projections=1, predicates=0, joins=0, ctes=0))", Arel.inspect(children[0]))
    Minitest.assert_equal("Attribute(people.name)", Arel.inspect(children[1]))
    Minitest.assert_equal("Join(INNER, Table(roles), Predicate(=, Attribute(people.role_id), Attribute(roles.id)))", Arel.inspect(children[2]))
    Minitest.assert_equal("Predicate(=, Attribute(people.active), Bind(true))", Arel.inspect(children[3]))
    Minitest.assert_equal("Ordering(ASC, Attribute(people.name))", Arel.inspect(children[4]))
  end

  suite = Minitest.new()
  suite.test("ordered expression children", test_expression_children_are_ordered)
  suite.test("ordered predicate children", test_predicate_children_preserve_semantic_order)
  suite.test("ordered query children", test_query_children_follow_render_order)
  suite.run()
end

run_tests()
