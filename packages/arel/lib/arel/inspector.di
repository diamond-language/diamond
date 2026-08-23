module Arel

class Inspector
def with_children(node, replacements: Array)
  if node is Function
    Function.new(node.name(), replacements, node.distinct?())
  elsif node is Cast
    if replacements.length() != 1
      raise ArgumentError.new("Cast requires exactly one child")
    end
    Cast.new(replacements[0], node.type_name())
  elsif node is Collation
    if replacements.length() != 1
      raise ArgumentError.new("Collation requires exactly one child")
    end
    Collation.new(replacements[0], node.name())
  elsif node is Alias
    if replacements.length() != 1
      raise ArgumentError.new("Alias requires exactly one child")
    end
    Alias.new(replacements[0], node.name())
  elsif node is Ordering
    if replacements.length() != 1
      raise ArgumentError.new("Ordering requires exactly one child")
    end
    Ordering.new(replacements[0], node.direction(), node.nulls())
  elsif node is AssignmentValue
    if replacements.length() != 1
      raise ArgumentError.new("AssignmentValue requires exactly one child")
    end
    AssignmentValue.new(replacements[0])
  else
    self.with_children_tail(node, replacements)
  end
end

def with_children_tail(node, replacements: Array)
  if node is BinaryExpression
    expected = 1
    unless node.bind_right?()
      expected = 2
    end
    if replacements.length() != expected
      raise ArgumentError.new("BinaryExpression replacement child count mismatch")
    end
    right = node.right()
    unless node.bind_right?()
      right = replacements[1]
    end
    BinaryExpression.new(replacements[0], node.operator(), right, node.bind_right?())
  elsif node is QualifiedStar
    if replacements.length() != 1 || !(replacements[0] is Table)
      raise ArgumentError.new("QualifiedStar requires exactly one table child")
    end
    QualifiedStar.new(replacements[0])
  elsif node is Predicate
    expected = 1
    structural_right = node.right() is Attribute || node.right() is Literal
    if structural_right
      expected = 2
    end
    if replacements.length() != expected
      raise ArgumentError.new("Predicate replacement child count mismatch")
    end
    right = node.right()
    if structural_right
      right = replacements[1]
    end
    Predicate.new(replacements[0], node.operator(), right)
  elsif node is Logical
    if replacements.length() != 2
      raise ArgumentError.new("Logical requires exactly two children")
    end
    Logical.new(replacements[0], node.operator(), replacements[1])
  elsif node is Not
    if replacements.length() != 1
      raise ArgumentError.new("Not requires exactly one child")
    end
    Not.new(replacements[0])
  elsif node is Between
    if replacements.length() != 1
      raise ArgumentError.new("Between requires exactly one child")
    end
    Between.new(replacements[0], node.lower(), node.upper(), node.negated?())
  elsif node is Membership
    expected = 1
    unless node.values() is Array
      expected = 2
    end
    if replacements.length() != expected
      raise ArgumentError.new("Membership replacement child count mismatch")
    end
    values = node.values()
    unless values is Array
      values = replacements[1]
    end
    Membership.new(replacements[0], values, node.negated?())
  elsif node is Exists
    if replacements.length() != 1
      raise ArgumentError.new("Exists requires exactly one child")
    end
    Exists.new(replacements[0], node.negated?())
  elsif node is ScalarSubquery
    if replacements.length() != 1
      raise ArgumentError.new("ScalarSubquery requires exactly one child")
    end
    ScalarSubquery.new(replacements[0])
  elsif node is Join
    expected = 1
    unless node.predicate() == nil
      expected = 2
    end
    if replacements.length() != expected || !(replacements[0] is Table)
      raise ArgumentError.new("Join replacement children do not match its shape")
    end
    predicate = nil
    if expected == 2
      predicate = replacements[1]
    end
    Join.new(replacements[0], predicate, node.kind())
  elsif node is Cte
    if replacements.length() != 1
      raise ArgumentError.new("Cte requires exactly one child")
    end
    Cte.new(node.name(), replacements[0], node.recursive?())
  elsif node is ConflictTarget
    expected = 0
    unless node.predicate() == nil
      expected = 1
    end
    if replacements.length() != expected
      raise ArgumentError.new("ConflictTarget replacement child count mismatch")
    end
    predicate = nil
    if expected == 1
      predicate = replacements[0]
    end
    ConflictTarget.new(node.columns(), predicate)
  elsif node is CompoundQuery
    if replacements.length() != 2 + node.orderings().length()
      raise ArgumentError.new("CompoundQuery replacement child count mismatch")
    end
    orderings = []
    index = 2
    while index < replacements.length()
      orderings.push(replacements[index])
      index += 1
    end
    CompoundQuery.new(replacements[0], node.operator(), replacements[1], orderings,
      node.limit_value(), node.offset_value())
  elsif node is Query
    self.with_query_children(node, replacements)
  elsif node is Update || node is Delete || node is Insert
    self.with_write_children(node, replacements)
  elsif node is ArelReplaceableNode
    node.arel_with_children(replacements)
  else
    raise ArgumentError.new("Arel node does not support child replacement")
  end
end

def with_write_children(node, replacements: Array)
  if replacements.length() != self.children(node).length()
    raise ArgumentError.new("Arel write manager replacement child count mismatch")
  end
  if node is Update
    state = node.structure()
    index = state[5].length()
    table = replacements[index]
    index += 1
    assignments = nil
    unless state[1] == nil
      assignments = {}
      assignment_index = 0
      while assignment_index < state[1].length()
        key = state[1].key_at(assignment_index)
        value = state[1][key]
        if value is AssignmentValue
          value = replacements[index]
          index += 1
        end
        assignments[key] = value
        assignment_index += 1
      end
    end
    predicates = []
    predicate_index = 0
    while predicate_index < state[2].length()
      predicates.push(replacements[index])
      index += 1
      predicate_index += 1
    end
    returning = []
    returning_index = 0
    while returning_index < state[3].length()
      returning.push(replacements[index])
      index += 1
      returning_index += 1
    end
    ctes = []
    cte_index = 0
    while cte_index < state[5].length()
      ctes.push(replacements[cte_index])
      cte_index += 1
    end
    Update.new(table, assignments, predicates, returning, state[4], ctes)
  elsif node is Delete
    state = node.structure()
    index = state[4].length()
    table = replacements[index]
    index += 1
    predicates = []
    predicate_index = 0
    while predicate_index < state[1].length()
      predicates.push(replacements[index])
      index += 1
      predicate_index += 1
    end
    returning = []
    returning_index = 0
    while returning_index < state[2].length()
      returning.push(replacements[index])
      index += 1
      returning_index += 1
    end
    ctes = []
    cte_index = 0
    while cte_index < state[4].length()
      ctes.push(replacements[cte_index])
      cte_index += 1
    end
    Delete.new(table, predicates, returning, state[3], ctes)
  elsif node is Insert
    state = node.structure()
    index = state[8].length()
    table = replacements[index]
    index += 1
    source_query = state[4]
    unless source_query == nil
      source_query = replacements[index]
      index += 1
    end
    rows = []
    row_index = 0
    while row_index < state[1].length()
      original_row = state[1][row_index]
      if original_row is DefaultValues
        rows.push(original_row)
      else
        row = {}
        value_index = 0
        while value_index < original_row.length()
          key = original_row.key_at(value_index)
          value = original_row[key]
          if value is AssignmentValue
            value = replacements[index]
            index += 1
          end
          row[key] = value
          value_index += 1
        end
        rows.push(row)
      end
      row_index += 1
    end
    conflict_target = state[5]
    if conflict_target is ConflictTarget
      conflict_target = replacements[index]
      index += 1
    end
    conflict_assignments = nil
    unless state[7] == nil
      conflict_assignments = {}
      value_index = 0
      while value_index < state[7].length()
        key = state[7].key_at(value_index)
        value = state[7][key]
        if value is AssignmentValue
          value = replacements[index]
          index += 1
        end
        conflict_assignments[key] = value
        value_index += 1
      end
    end
    returning = []
    returning_index = 0
    while returning_index < state[2].length()
      returning.push(replacements[index])
      index += 1
      returning_index += 1
    end
    ctes = []
    cte_index = 0
    while cte_index < state[8].length()
      ctes.push(replacements[cte_index])
      cte_index += 1
    end
    Insert.new(table, rows, returning, state[3], source_query, conflict_target,
      state[6], conflict_assignments, ctes)
  else
    raise ArgumentError.new("Arel write manager does not support child replacement")
  end
end

def with_query_children(node: Arel::Query, replacements: Array)
  if replacements.length() != self.children(node).length()
    raise ArgumentError.new("Query replacement child count mismatch")
  end
  index = 0
  ctes = []
  part = 0
  while part < node.ctes().length()
    ctes.push(replacements[index + part])
    part += 1
  end
  index += node.ctes().length()
  source_query = nil
  unless node.source_query() == nil
    source_query = replacements[index]
    index += 1
  end
  projections = []
  part = 0
  while part < node.projections().length()
    projections.push(replacements[index + part])
    part += 1
  end
  index += node.projections().length()
  joins = []
  part = 0
  while part < node.joins().length()
    joins.push(replacements[index + part])
    part += 1
  end
  index += node.joins().length()
  predicates = []
  part = 0
  while part < node.predicates().length()
    predicates.push(replacements[index + part])
    part += 1
  end
  index += node.predicates().length()
  groups = []
  part = 0
  while part < node.groups().length()
    groups.push(replacements[index + part])
    part += 1
  end
  index += node.groups().length()
  havings = []
  part = 0
  while part < node.havings().length()
    havings.push(replacements[index + part])
    part += 1
  end
  index += node.havings().length()
  orderings = []
  part = 0
  while part < node.orderings().length()
    orderings.push(replacements[index + part])
    part += 1
  end
  index += node.orderings().length()
  correlations = []
  part = 0
  while part < node.correlations().length()
    correlations.push(replacements[index + part])
    part += 1
  end
  Query.new(node.table_name(), predicates, orderings, node.limit_value(),
    node.offset_value(), projections, node.quoted_identifiers(), node.bind_limits(),
    node.table_alias(), node.distinct_value(), groups, havings, joins, source_query,
    correlations, ctes)
end

def simplify(node, rules = [], report = false)
  original = node
  children = self.children(node)
  if children.length() > 0
    replacements = []
    index = 0
    while index < children.length()
      replacements.push(self.simplify(children[index], rules))
      index += 1
    end
    node = self.with_children(node, replacements)
  end
  if node is Not && node.expression() is Not
    node = node.expression().expression()
  elsif node is Membership && node.values() is Array && node.values().length() == 0
    if node.negated?()
      node = RawSql.new("1 = 1", [])
    else
      node = RawSql.new("1 = 0", [])
    end
  end
  rule_index = 0
  while rule_index < rules.length()
    rule = rules[rule_index]
    if !(rule is Array) || rule.length() != 2
      raise ArgumentError.new("Arel rewrite rule must be [pattern, replacement]")
    end
    if self.same?(node, rule[0])
      node = rule[1]
    end
    rule_index += 1
  end
  if report
    [node, !self.same?(original, node)]
  else
    node
  end
end

def walk(node, visitor = nil) -> Array
  visited = []
  pending = [node]
  while pending.length() > 0
    current = pending.pop()
    visited.push(current)
    unless visitor == nil
      visitor.visit(current)
    end
    children = self.children(current)
    index = children.length()
    while index > 0
      index -= 1
      pending.push(children[index])
    end
  end
  visited
end

def children(node) -> Array
  if node is Attribute || node is Literal || node is ExcludedAttribute ||
     node is RawSql || node is Table || node is ConflictAttribute ||
     node is DefaultValues
    []
  elsif node is BinaryExpression
    children = [node.left()]
    unless node.bind_right?()
      children.push(node.right())
    end
    children
  elsif node is Function
    node.arguments()
  elsif node is Cast || node is Collation || node is Alias ||
        node is Ordering || node is AssignmentValue
    [node.expression()]
  elsif node is QualifiedStar
    [node.table()]
  elsif node is Predicate
    children = [node.left()]
    if node.right() is Attribute || node.right() is Literal
      children.push(node.right())
    end
    children
  elsif node is Logical
    [node.left(), node.right()]
  elsif node is Not
    [node.expression()]
  elsif node is Between
    [node.left()]
  elsif node is Membership
    children = [node.left()]
    unless node.values() is Array
      children.push(node.values())
    end
    children
  elsif node is Exists || node is ScalarSubquery
    [node.query()]
  elsif node is Join
    children = [node.table()]
    unless node.predicate() == nil
      children.push(node.predicate())
    end
    children
  elsif node is ArelTraversalNode
    node.arel_children()
  else
    self.children_tail(node)
  end
end

def children_tail(node) -> Array
  if node is Query
    children = [].concat(node.ctes())
    unless node.source_query() == nil
      children.push(node.source_query())
    end
    children = children.concat(node.projections())
    children = children.concat(node.joins())
    children = children.concat(node.predicates())
    children = children.concat(node.groups())
    children = children.concat(node.havings())
    children = children.concat(node.orderings())
    children.concat(node.correlations())
  elsif node is CompoundQuery
    [node.left(), node.right()].concat(node.orderings())
  elsif node is Cte
    [node.query()]
  elsif node is ConflictTarget
    if node.predicate() == nil
      []
    else
      [node.predicate()]
    end
  elsif node is Insert || node is Update || node is Delete
    self.children_write(node)
  else
    []
  end
end

def children_write(node) -> Array
  if node is Insert
    state = node.structure()
    children = [].concat(state[8])
    children.push(state[0])
    unless state[4] == nil
      children.push(state[4])
    end
    row_index = 0
    while row_index < state[1].length()
      row = state[1][row_index]
      unless row is DefaultValues
        value_index = 0
        while value_index < row.length()
          value = row[row.key_at(value_index)]
          if value is AssignmentValue
            children.push(value)
          end
          value_index += 1
        end
      end
      row_index += 1
    end
    if state[5] is ConflictTarget
      children.push(state[5])
    end
    unless state[7] == nil
      value_index = 0
      while value_index < state[7].length()
        value = state[7][state[7].key_at(value_index)]
        if value is AssignmentValue
          children.push(value)
        end
        value_index += 1
      end
    end
    children.concat(state[2])
  elsif node is Update
    state = node.structure()
    children = [].concat(state[5])
    children.push(state[0])
    unless state[1] == nil
      value_index = 0
      while value_index < state[1].length()
        value = state[1][state[1].key_at(value_index)]
        if value is AssignmentValue
          children.push(value)
        end
        value_index += 1
      end
    end
    children = children.concat(state[2])
    children.concat(state[3])
  elsif node is Delete
    state = node.structure()
    children = [].concat(state[4])
    children.push(state[0])
    children = children.concat(state[1])
    children.concat(state[2])
  else
    []
  end
end

def inspect(node) -> String
  if node is Attribute
    "Attribute(#{node.table().reference_name()}.#{node.name()})"
  elsif node is BinaryExpression
    right = "Bind(#{node.right()})"
    unless node.bind_right?()
      right = self.inspect(node.right())
    end
    "Binary(#{node.operator()}, #{self.inspect(node.left())}, #{right})"
  elsif node is Literal
    "Literal(#{node.value()})"
  elsif node is Function
    arguments = []
    index = 0
    while index < node.arguments().length()
      arguments.push(self.inspect(node.arguments()[index]))
      index += 1
    end
    "Function(#{node.name()}, [#{arguments.join(", ")}])"
  elsif node is Cast
    "Cast(#{self.inspect(node.expression())}, #{node.type_name()})"
  elsif node is ExcludedAttribute
    "Excluded(#{node.name()})"
  elsif node is Table
    if node.table_alias() == nil
      "Table(#{node.name()})"
    else
      "Table(#{node.name()} AS #{node.table_alias()})"
    end
  elsif node is QualifiedStar
    "QualifiedStar(#{node.table().reference_name()})"
  elsif node is ConflictAttribute
    "ConflictAttribute(#{node.name()})"
  elsif node is Exists
    prefix = "Exists"
    if node.negated?()
      prefix = "NotExists"
    end
    "#{prefix}(#{self.inspect(node.query())})"
  elsif node is ScalarSubquery
    "Scalar(#{self.inspect(node.query())})"
  elsif node is AssignmentValue
    "Assignment(#{self.inspect(node.expression())})"
  elsif node is ConflictTarget
    columns = node.columns().join(", ")
    "ConflictTarget(#{columns}, predicate=#{node.predicate() != nil})"
  elsif node is DefaultValues
    "DefaultValues"
  elsif node is RawSql
    "RawSql(#{node.sql()}, #{node.params().length()} binds)"
  elsif node is Collation
    "Collation(#{node.name()}, #{self.inspect(node.expression())})"
  else
    self.inspect_tail(node)
  end
end

def inspect_tail(node) -> String
  if node is Predicate
    right = "Bind(#{node.right()})"
    if node.right() is Attribute || node.right() is Literal
      right = self.inspect(node.right())
    end
    "Predicate(#{node.operator()}, #{self.inspect(node.left())}, #{right})"
  elsif node is Logical
    "Logical(#{node.operator()}, #{self.inspect(node.left())}, #{self.inspect(node.right())})"
  elsif node is Not
    "Not(#{self.inspect(node.expression())})"
  elsif node is Between
    operator = "BETWEEN"
    if node.negated?()
      operator = "NOT BETWEEN"
    end
    "Between(#{operator}, #{self.inspect(node.left())}, #{node.lower()}, #{node.upper()})"
  elsif node is Membership
    operator = "IN"
    if node.negated?()
      operator = "NOT IN"
    end
    if node.values() is Array
      "Membership(#{operator}, #{self.inspect(node.left())}, #{node.values().length()} values)"
    else
      "Membership(#{operator}, #{self.inspect(node.left())}, subquery)"
    end
  elsif node is Ordering
    nulls = ""
    unless node.nulls() == nil
      nulls = ", NULLS #{node.nulls()}"
    end
    "Ordering(#{node.direction()}#{nulls}, #{self.inspect(node.expression())})"
  elsif node is Alias
    "Alias(#{node.name()}, #{self.inspect(node.expression())})"
  elsif node is Join
    predicate = "none"
    unless node.predicate() == nil
      predicate = self.inspect(node.predicate())
    end
    "Join(#{node.kind()}, #{self.inspect(node.table())}, #{predicate})"
  elsif node is Cte
    mode = "ordinary"
    if node.recursive?()
      mode = "recursive"
    end
    "Cte(#{node.name()}, #{mode}, #{self.inspect(node.query())})"
  elsif node is Query
    "Query(from=#{node.base_reference_name()}, projections=#{node.projections().length()}, predicates=#{node.predicates().length()}, joins=#{node.joins().length()}, ctes=#{node.ctes().length()})"
  elsif node is CompoundQuery
    "Compound(#{node.operator()}, #{self.inspect(node.left())}, #{self.inspect(node.right())})"
  elsif node is Insert
    state = node.structure()
    source = state[4] != nil
    "Insert(into=#{state[0].reference_name()}, rows=#{state[1].length()}, source=#{source}, returning=#{state[2].length()}, ctes=#{state[8].length()})"
  elsif node is Update
    state = node.structure()
    assignments = 0
    unless state[1] == nil
      assignments = state[1].length()
    end
    "Update(table=#{state[0].reference_name()}, assignments=#{assignments}, predicates=#{state[2].length()}, returning=#{state[3].length()}, all=#{state[4]}, ctes=#{state[5].length()})"
  elsif node is Delete
    state = node.structure()
    "Delete(from=#{state[0].reference_name()}, predicates=#{state[1].length()}, returning=#{state[2].length()}, all=#{state[3]}, ctes=#{state[4].length()})"
  elsif node is ArelInspectable
    node.arel_inspect()
  else
    "ArelNode(unknown)"
  end
end

def same?(left, right) -> Bool
  if left is Attribute
    right is Attribute && left.table().reference_name() == right.table().reference_name() &&
      left.name() == right.name()
  elsif left is BinaryExpression
    if !(right is BinaryExpression) || left.operator() != right.operator() ||
       left.bind_right?() != right.bind_right?() || !self.same?(left.left(), right.left())
      false
    elsif left.bind_right?()
      left.right() == right.right()
    else
      self.same?(left.right(), right.right())
    end
  elsif left is Literal
    right is Literal && left.value() == right.value()
  elsif left is Cast
    right is Cast && left.type_name() == right.type_name() &&
      self.same?(left.expression(), right.expression())
  elsif left is ExcludedAttribute
    right is ExcludedAttribute && left.name() == right.name()
  elsif left is Function
    if !(right is Function) || left.name() != right.name() ||
       left.distinct?() != right.distinct?() || left.arguments().length() != right.arguments().length()
      return false
    end
    index = 0
    while index < left.arguments().length()
      unless self.same?(left.arguments()[index], right.arguments()[index])
        return false
      end
      index += 1
    end
    true
  elsif left is RawSql
    if !(right is RawSql) || left.sql() != right.sql() ||
       left.params().length() != right.params().length()
      return false
    end
    index = 0
    while index < left.params().length()
      if left.params()[index] != right.params()[index]
        return false
      end
      index += 1
    end
    true
  elsif left is AssignmentValue
    right is AssignmentValue && self.same?(left.expression(), right.expression())
  elsif left is DefaultValues
    right is DefaultValues
  elsif left is QualifiedStar
    right is QualifiedStar && self.same?(left.table(), right.table())
  elsif left is ConflictAttribute
    right is ConflictAttribute && left.name() == right.name()
  elsif left is Exists
    right is Exists && left.negated?() == right.negated?() &&
      self.same?(left.query(), right.query())
  elsif left is ScalarSubquery
    right is ScalarSubquery && self.same?(left.query(), right.query())
  elsif left is ConflictTarget
    if !(right is ConflictTarget) || left.columns().length() != right.columns().length() ||
       (left.predicate() == nil) != (right.predicate() == nil)
      return false
    end
    index = 0
    while index < left.columns().length()
      if left.columns()[index] != right.columns()[index]
        return false
      end
      index += 1
    end
    left.predicate() == nil || self.same?(left.predicate(), right.predicate())
  elsif left is Predicate
    if !(right is Predicate) || left.operator() != right.operator() ||
       !self.same?(left.left(), right.left())
      false
    elsif left.right() is Attribute || left.right() is Literal
      self.same?(left.right(), right.right())
    else
      left.right() == right.right()
    end
  elsif left is Logical
    right is Logical && left.operator() == right.operator() &&
      self.same?(left.left(), right.left()) && self.same?(left.right(), right.right())
  elsif left is Not
    right is Not && self.same?(left.expression(), right.expression())
  else
    self.same_tail?(left, right)
  end
end

def same_nodes?(left, right) -> Bool
  if left == nil || right == nil
    return left == nil && right == nil
  end
  if left.length() != right.length()
    return false
  end
  if left is Hash
    unless right is Hash
      return false
    end
    index = 0
    while index < left.length()
      key = left.key_at(index)
      unless right.include_key?(key)
        return false
      end
      left_value = left[key]
      right_value = right[key]
      if left_value is AssignmentValue
        unless self.same?(left_value, right_value)
          return false
        end
      elsif left_value != right_value
        return false
      end
      index += 1
    end
    return true
  end
  index = 0
  while index < left.length()
    unless self.same?(left[index], right[index])
      return false
    end
    index += 1
  end
  true
end

def same_insert?(left: Arel::Insert, right: Arel::Insert) -> Bool
  left_state = left.structure()
  right_state = right.structure()
  if !self.same?(left_state[0], right_state[0]) ||
     left_state[1].length() != right_state[1].length() ||
     !self.same_nodes?(left_state[2], right_state[2]) ||
     left_state[3].length() != right_state[3].length() || left_state[6] != right_state[6] ||
     !self.same_nodes?(left_state[7], right_state[7]) ||
     !self.same_nodes?(left_state[8], right_state[8]) ||
     (left_state[4] == nil) != (right_state[4] == nil)
    return false
  end
  index = 0
  while index < left_state[3].length()
    if left_state[3][index] != right_state[3][index]
      return false
    end
    index += 1
  end
  if left_state[4] != nil && !self.same?(left_state[4], right_state[4])
    return false
  end
  if (left_state[5] is Array) != (right_state[5] is Array)
    return false
  end
  if left_state[5] is Array
    if left_state[5].length() != right_state[5].length()
      return false
    end
    index = 0
    while index < left_state[5].length()
      if left_state[5][index] != right_state[5][index]
        return false
      end
      index += 1
    end
  elsif !self.same?(left_state[5], right_state[5])
    return false
  end
  index = 0
  while index < left_state[1].length()
    left_row = left_state[1][index]
    right_row = right_state[1][index]
    if left_row is DefaultValues
      unless self.same?(left_row, right_row)
        return false
      end
    elsif !self.same_nodes?(left_row, right_row)
      return false
    end
    index += 1
  end
  true
end

def same_tail?(left, right) -> Bool
  if left is Between
    right is Between && left.negated?() == right.negated?() &&
      left.lower() == right.lower() && left.upper() == right.upper() &&
      self.same?(left.left(), right.left())
  elsif left is Membership
    if !(right is Membership) || left.negated?() != right.negated?() ||
       !self.same?(left.left(), right.left()) ||
       (left.values() is Array) != (right.values() is Array)
      return false
    end
    unless left.values() is Array
      return self.same?(left.values(), right.values())
    end
    if left.values().length() != right.values().length()
      return false
    end
    index = 0
    while index < left.values().length()
      if left.values()[index] != right.values()[index]
        return false
      end
      index += 1
    end
    true
  elsif left is Ordering
    right is Ordering && left.direction() == right.direction() &&
      left.nulls() == right.nulls() && self.same?(left.expression(), right.expression())
  elsif left is Alias
    right is Alias && left.name() == right.name() &&
      self.same?(left.expression(), right.expression())
  elsif left is Collation
    right is Collation && left.name() == right.name() &&
      self.same?(left.expression(), right.expression())
  elsif left is Join
    right is Join && left.kind() == right.kind() &&
      self.same?(left.table(), right.table()) &&
      ((left.predicate() == nil && right.predicate() == nil) ||
       (left.predicate() != nil && right.predicate() != nil &&
        self.same?(left.predicate(), right.predicate())))
  elsif left is Table
    right is Table && left.name() == right.name() &&
      left.table_alias() == right.table_alias()
  elsif left is Cte
    right is Cte && left.name() == right.name() &&
      left.recursive?() == right.recursive?() && self.same?(left.query(), right.query())
  elsif left is CompoundQuery
    if !(right is CompoundQuery) || left.operator() != right.operator() ||
       left.limit_value() != right.limit_value() || left.offset_value() != right.offset_value() ||
       !self.same?(left.left(), right.left()) || !self.same?(left.right(), right.right())
      return false
    end
    self.same_nodes?(left.orderings(), right.orderings())
  elsif left is Update
    unless right is Update
      return false
    end
    left_state = left.structure()
    right_state = right.structure()
    self.same?(left_state[0], right_state[0]) &&
      self.same_nodes?(left_state[1], right_state[1]) &&
      self.same_nodes?(left_state[2], right_state[2]) &&
      self.same_nodes?(left_state[3], right_state[3]) &&
      left_state[4] == right_state[4] && self.same_nodes?(left_state[5], right_state[5])
  elsif left is Insert
    right is Insert && self.same_insert?(left, right)
  elsif left is Delete
    unless right is Delete
      return false
    end
    left_state = left.structure()
    right_state = right.structure()
    self.same?(left_state[0], right_state[0]) &&
      self.same_nodes?(left_state[1], right_state[1]) &&
      self.same_nodes?(left_state[2], right_state[2]) &&
      left_state[3] == right_state[3] && self.same_nodes?(left_state[4], right_state[4])
  elsif left is Query
    if !(right is Query) || left.base_reference_name() != right.base_reference_name() ||
       left.distinct_value() != right.distinct_value() ||
       left.limit_value() != right.limit_value() || left.offset_value() != right.offset_value() ||
       !self.same_nodes?(left.projections(), right.projections()) ||
       !self.same_nodes?(left.predicates(), right.predicates()) ||
       !self.same_nodes?(left.orderings(), right.orderings()) ||
       !self.same_nodes?(left.groups(), right.groups()) ||
       !self.same_nodes?(left.havings(), right.havings()) ||
       !self.same_nodes?(left.correlations(), right.correlations()) ||
       !self.same_nodes?(left.joins(), right.joins()) ||
       !self.same_nodes?(left.ctes(), right.ctes())
      return false
    end
    if (left.source_query() == nil) != (right.source_query() == nil)
      return false
    end
    if left.source_query() != nil && !self.same?(left.source_query(), right.source_query())
      return false
    end
    true
  elsif left is ArelComparableNode
    left.arel_same?(right)
  else
    false
  end
end
end

end
