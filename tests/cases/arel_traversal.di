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

class ThirdPartyPair
  def initialize(left, right)
    @left = left
    @right = right
  end
  def arel_children() -> Array = [@left, @right]
  def arel_inspect() -> String = "ThirdPartyPair"
  def arel_same?(other) -> Bool
    if !(other is ThirdPartyPair)
      return false
    end
    other_children = other.arel_children()
    Arel.same?(@left, other_children[0]) && Arel.same?(@right, other_children[1])
  end
  def arel_with_children(replacements: Array) = ThirdPartyPair.new(replacements[0], replacements[1])
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

  def test_expression_children_can_be_replaced_immutably()
    people = Arel.table("people")
    original = Arel.lower(people.column("name"))
    replaced = Arel.with_children(original, [people.column("email")])
    Minitest.assert_equal("Function(LOWER, [Attribute(people.name)])", Arel.inspect(original))
    Minitest.assert_equal("Function(LOWER, [Attribute(people.email)])", Arel.inspect(replaced))
    ordering = Arel.asc(people.column("name")).nulls_last()
    changed = Arel.with_children(ordering, [people.column("email")])
    Minitest.assert_equal("Ordering(ASC, NULLS LAST, Attribute(people.email))", Arel.inspect(changed))
    binary = people.column("score").add_expression(Arel.literal(1))
    changed_binary = Arel.with_children(binary, [people.column("rank"), Arel.literal(2)])
    Minitest.assert_equal("Binary(+, Attribute(people.rank), Literal(2))", Arel.inspect(changed_binary))
    bound = people.column("score").add(1)
    changed_bound = Arel.with_children(bound, [people.column("rank")])
    Minitest.assert_equal("Binary(+, Attribute(people.rank), Bind(1))", Arel.inspect(changed_bound))
  end

  def test_replacement_rejects_shape_mismatches()
    people = Arel.table("people")
    raised = false
    begin
      Arel.with_children(people.column("name").asc(), [])
    rescue ArgumentError
      raised = true
    end
    Minitest.assert_equal(true, raised)
    raised = false
    begin
      Arel.with_children(people.column("age").between(18, 65),
        [people.column("age"), Arel.literal(18)])
    rescue ArgumentError
      raised = true
    end
    Minitest.assert_equal(true, raised)
    raised = false
    begin
      Arel.with_children(people, [])
    rescue ArgumentError
      raised = true
    end
    Minitest.assert_equal(true, raised)
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

  def test_predicate_children_can_be_replaced_immutably()
    people = Arel.table("people")
    original = people.column("active").eq(true).and_also(people.column("age").gt(18))
    replacement = people.column("verified").eq(true)
    changed = Arel.with_children(original, [replacement, Arel.children(original)[1]])
    Minitest.assert_equal("Logical(AND, Predicate(=, Attribute(people.active), Bind(true)), Predicate(>, Attribute(people.age), Bind(18)))", Arel.inspect(original))
    Minitest.assert_equal("Logical(AND, Predicate(=, Attribute(people.verified), Bind(true)), Predicate(>, Attribute(people.age), Bind(18)))", Arel.inspect(changed))
    range = people.column("age").between(18, 65)
    Minitest.assert_equal("Between(BETWEEN, Attribute(people.score), 18, 65)",
      Arel.inspect(Arel.with_children(range, [people.column("score")])))
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

  def test_query_children_can_be_replaced_immutably()
    people = Arel.table("people")
    query = Arel.from(people).project(people.column("name"))
    query = query.where(people.column("active").eq(true)).order(people.column("name").asc())
    children = Arel.children(query)
    replacements = [people.column("email"), children[1], people.column("email").desc()]
    changed = Arel.with_children(query, replacements)
    original_sql, original_params = query.to_sql()
    changed_sql, changed_params = changed.to_sql()
    Minitest.assert_equal("SELECT \"people\".\"name\" FROM \"people\" WHERE \"people\".\"active\" = ? ORDER BY \"people\".\"name\" ASC", original_sql)
    Minitest.assert_equal("SELECT \"people\".\"email\" FROM \"people\" WHERE \"people\".\"active\" = ? ORDER BY \"people\".\"email\" DESC", changed_sql)
    Minitest.assert_equal(original_params.join("|"), changed_params.join("|"))
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

  def test_composition_children_can_be_replaced()
    people = Arel.table("people")
    roles = Arel.table("roles")
    accounts = Arel.table("accounts")
    join = ArelJoin.new(roles, people.column("role_id").eq(roles.column("id")), "INNER")
    replacement_predicate = people.column("account_id").eq(accounts.column("id"))
    changed_join = Arel.with_children(join, [accounts, replacement_predicate])
    Minitest.assert_equal("Join(INNER, Table(accounts), Predicate(=, Attribute(people.account_id), Attribute(accounts.id)))", Arel.inspect(changed_join))
    body = Arel.from(roles).project(roles.column("id"))
    changed_cte = Arel.with_children(ArelCte.new("ids", body), [
      Arel.from(accounts).project(accounts.column("id"))
    ])
    Minitest.assert_equal("Cte(ids, ordinary, Query(from=accounts, projections=1, predicates=0, joins=0, ctes=0))", Arel.inspect(changed_cte))
    left = Arel.from(people).project(people.column("id"))
    right = Arel.from(roles).project(roles.column("id"))
    original = Arel.union(left, right).order(people.column("id").asc()).take(3)
    replacement_right = Arel.from(accounts).project(accounts.column("id"))
    changed = Arel.with_children(original, [left, replacement_right, accounts.column("id").desc()])
    sql, params = changed.to_sql()
    Minitest.assert_equal("SELECT \"people\".\"id\" FROM \"people\" UNION SELECT \"accounts\".\"id\" FROM \"accounts\" ORDER BY \"accounts\".\"id\" DESC LIMIT ?", sql)
    Minitest.assert_equal("3", params.join("|"))
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
    replacements = [children[0], Arel.expression(people.column("score").add(2)),
      children[2], Arel.expression(Arel.excluded("score").add(1)), people.column("email")]
    changed = Arel.with_children(insert, replacements)
    original_sql, original_params = insert.to_sql()
    changed_sql, changed_params = changed.to_sql()
    Minitest.assert_equal("INSERT INTO \"people\" (\"name\", \"score\") VALUES (?, (\"people\".\"score\" + ?)) ON CONFLICT (\"name\") DO UPDATE SET \"score\" = excluded.\"score\" RETURNING \"people\".\"id\"", original_sql)
    Minitest.assert_equal("INSERT INTO \"people\" (\"name\", \"score\") VALUES (?, (\"people\".\"score\" + ?)) ON CONFLICT (\"name\") DO UPDATE SET \"score\" = (excluded.\"score\" + ?) RETURNING \"people\".\"email\"", changed_sql)
    Minitest.assert_equal("Ada|1", original_params.join("|"))
    Minitest.assert_equal("Ada|2|1", changed_params.join("|"))
    source = Arel.from(people).project(people.column("name")).where(
      people.column("active").eq(true))
    source_insert = Arel.insert_into(Arel.table("archive")).from_query(["name"], source)
    source_children = Arel.children(source_insert)
    replacement_source = Arel.from(people).project(people.column("email")).where(
      people.column("active").eq(false))
    changed_source_insert = Arel.with_children(source_insert,
      [source_children[0], replacement_source])
    source_sql, source_params = changed_source_insert.to_sql()
    Minitest.assert_equal("INSERT INTO \"archive\" (\"name\") SELECT \"people\".\"email\" FROM \"people\" WHERE \"people\".\"active\" = ?", source_sql)
    Minitest.assert_equal("false", source_params.join("|"))
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
    update = Arel.update(people).set({
      "score": Arel.expression(people.column("score").add(1)),
      "active": true
    }).where(people.column("id").eq(7)).returning(people.column("score"))
    children = Arel.children(update)
    replacements = [children[0], Arel.expression(people.column("score").add(2)),
      people.column("id").eq(8), people.column("id")]
    changed = Arel.with_children(update, replacements)
    original_sql, original_params = update.to_sql()
    changed_sql, changed_params = changed.to_sql()
    Minitest.assert_equal("UPDATE \"people\" SET \"score\" = (\"people\".\"score\" + ?), \"active\" = ? WHERE \"people\".\"id\" = ? RETURNING \"people\".\"score\"", original_sql)
    Minitest.assert_equal("UPDATE \"people\" SET \"score\" = (\"people\".\"score\" + ?), \"active\" = ? WHERE \"people\".\"id\" = ? RETURNING \"people\".\"id\"", changed_sql)
    Minitest.assert_equal("1|true|7", original_params.join("|"))
    Minitest.assert_equal("2|true|8", changed_params.join("|"))
    Minitest.assert_equal("Update(table=people, assignments=2, predicates=1, returning=1, all=false, ctes=0)", Arel.inspect(update))
    delete = Arel.delete_from(people).where(people.column("inactive").eq(true)).returning(
      people.column("id"))
    delete_children = Arel.children(delete)
    changed_delete = Arel.with_children(delete, [delete_children[0],
      people.column("archived").eq(true), people.column("email")])
    delete_sql, delete_params = delete.to_sql()
    changed_delete_sql, changed_delete_params = changed_delete.to_sql()
    Minitest.assert_equal("DELETE FROM \"people\" WHERE \"people\".\"inactive\" = ? RETURNING \"people\".\"id\"", delete_sql)
    Minitest.assert_equal("DELETE FROM \"people\" WHERE \"people\".\"archived\" = ? RETURNING \"people\".\"email\"", changed_delete_sql)
    Minitest.assert_equal("true", delete_params.join("|"))
    Minitest.assert_equal("true", changed_delete_params.join("|"))
    source = Arel.from(people).project(people.column("id")).where(
      people.column("active").eq(true))
    cte_update = Arel.update(people).set({"active": false}).where(
      people.column("id").gt(10)).with("selected", source)
    cte_children = Arel.children(cte_update)
    replacement_source = Arel.from(people).project(people.column("id")).where(
      people.column("active").eq(false))
    replacement_cte = Arel.with_children(cte_children[0], [replacement_source])
    changed_cte_update = Arel.with_children(cte_update,
      [replacement_cte, cte_children[1], cte_children[2]])
    cte_sql, cte_params = changed_cte_update.to_sql()
    Minitest.assert_equal("WITH \"selected\" AS (SELECT \"people\".\"id\" FROM \"people\" WHERE \"people\".\"active\" = ?) UPDATE \"people\" SET \"active\" = ? WHERE \"people\".\"id\" > ?", cte_sql)
    Minitest.assert_equal("false|false|10", cte_params.join("|"))
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

  def test_third_party_nodes_opt_into_traversal()
    people = Arel.table("people")
    extension = ThirdPartyPair.new(people.column("name"), people.column("email"))
    Minitest.assert_equal("ThirdPartyPair", Arel.inspect(extension))
    same_extension = ThirdPartyPair.new(Arel.table("people").column("name"),
      Arel.table("people").column("email"))
    Minitest.assert_equal(true, Arel.same?(extension, same_extension))
    Minitest.assert_equal(false, Arel.same?(extension, ThirdPartyPair.new(
      people.column("name"), people.column("phone"))))
    changed = Arel.with_children(extension, [people.column("id"), people.column("email")])
    changed_children = Arel.children(changed)
    Minitest.assert_equal("Attribute(people.id)|Attribute(people.email)",
      Arel.inspect(changed_children[0]) + "|" + Arel.inspect(changed_children[1]))
    children = Arel.children(extension)
    Minitest.assert_equal("Attribute(people.name)|Attribute(people.email)",
      Arel.inspect(children[0]) + "|" + Arel.inspect(children[1]))
    Minitest.assert_equal(3, Arel.walk(extension).length())
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
    nested = people.column("id").gt(0).and_also(ArelNot.new(ArelNot.new(predicate)))
    nested_simple = Arel.simplify(nested)
    Minitest.assert_equal("Logical(AND, Predicate(>, Attribute(people.id), Bind(0)), Predicate(=, Attribute(people.active), Bind(true)))", Arel.inspect(nested_simple))
    query = Arel.from(people).where(nested)
    simple_query = Arel.simplify(query)
    sql, params = simple_query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE (\"people\".\"id\" > ? AND \"people\".\"active\" = ?)", sql)
    Minitest.assert_equal("0|true", params.join("|"))
    update = Arel.update(people).set({"active": true}).where(
      ArelNot.new(ArelNot.new(people.column("id").eq(4))))
    simple_update = Arel.simplify(update)
    update_sql, update_params = simple_update.to_sql()
    Minitest.assert_equal("UPDATE \"people\" SET \"active\" = ? WHERE \"people\".\"id\" = ?", update_sql)
    Minitest.assert_equal("true|4", update_params.join("|"))
    Minitest.assert_equal("Not(Not(Predicate(=, Attribute(people.id), Bind(4))))",
      Arel.inspect(Arel.children(update)[1]))
    empty_membership = people.column("id").in_list([])
    empty_simple = Arel.simplify(empty_membership)
    Minitest.assert_equal("RawSql(1 = 0, 0 binds)", Arel.inspect(empty_simple))
    original_empty_sql, original_empty_params = Arel.from(people).where(empty_membership).to_sql()
    simple_empty_sql, simple_empty_params = Arel.from(people).where(empty_simple).to_sql()
    Minitest.assert_equal(original_empty_sql, simple_empty_sql)
    Minitest.assert_equal(original_empty_params.length(), simple_empty_params.length())
    policy_source = people.column("active").eq(true)
    policy_replacement = people.column("verified").eq(true)
    policy_result = Arel.simplify(policy_source, [[policy_source, policy_replacement]])
    Minitest.assert_equal("Predicate(=, Attribute(people.verified), Bind(true))",
      Arel.inspect(policy_result))
    policy_query = Arel.from(people).where(policy_source.and_also(
      people.column("age").gt(18)))
    policy_query_result = Arel.simplify(policy_query,
      [[policy_source, policy_replacement]])
    policy_sql, policy_params = policy_query_result.to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE (\"people\".\"verified\" = ? AND \"people\".\"age\" > ?)", policy_sql)
    Minitest.assert_equal("true|18", policy_params.join("|"))
    reported_query, query_changed = Arel.simplify(policy_query,
      [[policy_source, policy_replacement]], true)
    Minitest.assert_equal(true, query_changed)
    Minitest.assert_equal(policy_sql, reported_query.to_sql()[0])
    original_policy_sql, original_policy_params = policy_query.to_sql()
    Minitest.assert_equal("SELECT * FROM \"people\" WHERE (\"people\".\"active\" = ? AND \"people\".\"age\" > ?)",
      original_policy_sql)
    Minitest.assert_equal("true|18", original_policy_params.join("|"))
    first = people.column("first_choice").eq(true)
    second = people.column("second_choice").eq(true)
    first_result = Arel.simplify(policy_source,
      [[policy_source, first], [policy_source, second]])
    Minitest.assert_equal("Predicate(=, Attribute(people.first_choice), Bind(true))",
      Arel.inspect(first_result))
    invalid_raised = false
    begin
      Arel.simplify(policy_source, [[policy_source]])
    rescue ArgumentError
      invalid_raised = true
    end
    Minitest.assert_equal(true, invalid_raised)
    reported, changed = Arel.simplify(policy_source,
      [[policy_source, policy_replacement]], true)
    Minitest.assert_equal(true, changed)
    Minitest.assert_equal("Predicate(=, Attribute(people.verified), Bind(true))",
      Arel.inspect(reported))
    unchanged, changed = Arel.simplify(policy_source, [], true)
    Minitest.assert_equal(false, changed)
    Minitest.assert_equal(true, Arel.same?(policy_source, unchanged))
    final = people.column("final_choice").eq(true)
    composed = Arel.simplify(policy_source,
      [[policy_source, first], [first, second], [second, final]])
    Minitest.assert_equal("Predicate(=, Attribute(people.final_choice), Bind(true))",
      Arel.inspect(composed))
    policy_update = Arel.update(people).set({"score": 7}).where(policy_source)
    rewritten_update = Arel.simplify(policy_update,
      [[policy_source, people.column("verified").eq(false)]])
    rewrite_sql, rewrite_params = rewritten_update.to_sql()
    Minitest.assert_equal("UPDATE \"people\" SET \"score\" = ? WHERE \"people\".\"verified\" = ?",
      rewrite_sql)
    Minitest.assert_equal("7|false", rewrite_params.join("|"))
    extension = ThirdPartyPair.new(policy_source, people.column("name"))
    rewritten_extension = Arel.simplify(extension,
      [[policy_source, policy_replacement]])
    extension_children = Arel.children(rewritten_extension)
    Minitest.assert_equal("Predicate(=, Attribute(people.verified), Bind(true))",
      Arel.inspect(extension_children[0]))
    Minitest.assert_equal("Attribute(people.name)", Arel.inspect(extension_children[1]))
    composed = Arel.simplify(ArelNot.new(ArelNot.new(policy_source)),
      [[policy_source, policy_replacement]])
    Minitest.assert_equal("Predicate(=, Attribute(people.verified), Bind(true))",
      Arel.inspect(composed))
  end

  suite = Minitest.new()
  suite.test("ordered expression children", test_expression_children_are_ordered)
  suite.test("immutable expression child replacement", test_expression_children_can_be_replaced_immutably)
  suite.test("replacement shape validation", test_replacement_rejects_shape_mismatches)
  suite.test("ordered predicate children", test_predicate_children_preserve_semantic_order)
  suite.test("immutable predicate child replacement", test_predicate_children_can_be_replaced_immutably)
  suite.test("ordered query children", test_query_children_follow_render_order)
  suite.test("immutable query child replacement", test_query_children_can_be_replaced_immutably)
  suite.test("composition children", test_composition_children_are_structural)
  suite.test("immutable composition child replacement", test_composition_children_can_be_replaced)
  suite.test("ordered insert children", test_insert_children_follow_bind_structure)
  suite.test("ordered update and delete children", test_update_and_delete_children_are_ordered)
  suite.test("depth-first preorder walk", test_walk_is_depth_first_preorder)
  suite.test("third-party traversal protocol", test_third_party_nodes_opt_into_traversal)
  suite.test("immutable double-negation simplification", test_simplify_eliminates_double_negation_immutably)
  suite.run!()
end

run_tests()
