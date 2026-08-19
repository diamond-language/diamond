require "../../lib/minitest"
require "../../packages/arel/arel"

class RecordingVisitor
  def initialize()
    @nodes = []
  end
  def visit(node)
    @nodes.push(Arel.inspect(node))
  end
  def nodes() = @nodes
end

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

  def test_composition_children_are_structural()
    people = Arel.table("people")
    archived = Arel.table("archived")
    left = Arel.from(people).project(people.column("id"))
    right = Arel.from(archived).project(archived.column("id"))
    compound_children = Arel.children(Arel.union(left, right).order(people.column("id").asc()))
    Minitest.assert_equal(3, compound_children.length())
    Minitest.assert_equal("Query(from=people, projections=1, predicates=0, joins=0, ctes=0)", Arel.inspect(compound_children[0]))
    Minitest.assert_equal("Query(from=archived, projections=1, predicates=0, joins=0, ctes=0)", Arel.inspect(compound_children[1]))
    target = Arel.conflict_target(["id"]).where(people.column("active").eq(Arel.literal(true)))
    Minitest.assert_equal("Predicate(=, Attribute(people.active), Literal(true))", Arel.inspect(Arel.children(target)[0]))
  end

  def test_insert_children_follow_bind_structure()
    people = Arel.table("people")
    insert = Arel.insert_into(people).values({
      "name": "Ada",
      "score": Arel.expression(people.column("score").add(1))
    }).on_conflict_do_update(Arel.conflict_target(["name"]), {
      "score": Arel.expression(Arel.excluded("score"))
    }).returning(people.column("id"))
    children = Arel.children(insert)
    Minitest.assert_equal("Table(people)", Arel.inspect(children[0]))
    Minitest.assert_equal("Assignment(Binary(+, Attribute(people.score), Bind(1)))", Arel.inspect(children[1]))
    Minitest.assert_equal("ConflictTarget(name, predicate=false)", Arel.inspect(children[2]))
    Minitest.assert_equal("Assignment(Excluded(score))", Arel.inspect(children[3]))
    Minitest.assert_equal("Attribute(people.id)", Arel.inspect(children[4]))
  end

  def test_update_and_delete_children_are_ordered()
    people = Arel.table("people")
    update = Arel.update(people).set({
      "score": Arel.expression(people.column("score").add(1))
    }).where(people.column("id").eq(7)).returning(people.column("score"))
    update_children = Arel.children(update)
    Minitest.assert_equal("Table(people)", Arel.inspect(update_children[0]))
    Minitest.assert_equal("Assignment(Binary(+, Attribute(people.score), Bind(1)))", Arel.inspect(update_children[1]))
    Minitest.assert_equal("Predicate(=, Attribute(people.id), Bind(7))", Arel.inspect(update_children[2]))
    Minitest.assert_equal("Attribute(people.score)", Arel.inspect(update_children[3]))
    delete_children = Arel.children(Arel.delete_from(people).where(
      people.column("inactive").eq(true)).returning(people.column("id")))
    Minitest.assert_equal("Table(people)", Arel.inspect(delete_children[0]))
    Minitest.assert_equal("Predicate(=, Attribute(people.inactive), Bind(true))", Arel.inspect(delete_children[1]))
    Minitest.assert_equal("Attribute(people.id)", Arel.inspect(delete_children[2]))
  end

  def test_walk_is_depth_first_preorder()
    people = Arel.table("people")
    predicate = people.column("active").eq(Arel.literal(true)).and_also(
      people.column("age").gt(18))
    visited = Arel.walk(predicate)
    descriptions = []
    index = 0
    while index < visited.length()
      descriptions.push(Arel.inspect(visited[index]))
      index = index + 1
    end
    expected = [
      "Logical(AND, Predicate(=, Attribute(people.active), Literal(true)), Predicate(>, Attribute(people.age), Bind(18)))",
      "Predicate(=, Attribute(people.active), Literal(true))",
      "Attribute(people.active)",
      "Literal(true)",
      "Predicate(>, Attribute(people.age), Bind(18))",
      "Attribute(people.age)"
    ]
    Minitest.assert_equal(expected.join("|"), descriptions.join("|"))
    visitor = RecordingVisitor.new()
    returned = Arel.walk(predicate, visitor)
    Minitest.assert_equal(descriptions.join("|"), visitor.nodes().join("|"))
    Minitest.assert_equal(descriptions.length(), returned.length())
  end

  def test_simplify_eliminates_double_negation_immutably()
    people = Arel.table("people")
    predicate = people.column("active").eq(true)
    wrapped = ArelNot.new(ArelNot.new(predicate))
    simplified = Arel.simplify(wrapped)
    Minitest.assert_equal(true, Arel.same?(predicate, simplified))
    Minitest.assert_equal("Not(Not(Predicate(=, Attribute(people.active), Bind(true))))", Arel.inspect(wrapped))
    original_sql, original_params = Arel.from(people).where(wrapped).to_sql()
    simple_sql, simple_params = Arel.from(people).where(simplified).to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE (NOT (NOT \"people\".\"active\" = ?))", original_sql)
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE \"people\".\"active\" = ?", simple_sql)
    Minitest.assert_equal(original_params.join("|"), simple_params.join("|"))
  end

  suite = Minitest.new()
  suite.test("ordered expression children", test_expression_children_are_ordered)
  suite.test("ordered predicate children", test_predicate_children_preserve_semantic_order)
  suite.test("ordered query children", test_query_children_follow_render_order)
  suite.test("composition children", test_composition_children_are_structural)
  suite.test("ordered insert children", test_insert_children_follow_bind_structure)
  suite.test("ordered update and delete children", test_update_and_delete_children_are_ordered)
  suite.test("depth-first preorder walk", test_walk_is_depth_first_preorder)
  suite.test("immutable double-negation simplification", test_simplify_eliminates_double_negation_immutably)
  suite.run!()
end

run_tests()
