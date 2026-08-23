module Arel

  def self.table(name: String) = Table.new(name)
  def self.cte(name: String) = CteRelation.new(name)
  def self.as(expression, name: String) = Alias.new(expression, name)
  def self.asc(expression) = Ordering.new(expression, "ASC")
  def self.desc(expression) = Ordering.new(expression, "DESC")
  def self.sql(fragment: String, params = nil)
    bound = params
    if bound == nil
      bound = []
    end
    RawSql.new(fragment, bound)
  end
  def self.count(expression) = Function.new("COUNT", [expression])
  def self.count_distinct(expression) = Function.new("COUNT", [expression], true)
  def self.sum(expression) = Function.new("SUM", [expression])
  def self.min(expression) = Function.new("MIN", [expression])
  def self.max(expression) = Function.new("MAX", [expression])
  def self.avg(expression) = Function.new("AVG", [expression])
  def self.lower(expression) = Function.new("LOWER", [expression])
  def self.upper(expression) = Function.new("UPPER", [expression])
  def self.function(name: String, arguments: Array) = Function.new(name, arguments)
  def self.exists(query) = Exists.new(query, false)
  def self.not_exists(query) = Exists.new(query, true)
  def self.scalar(query) = ScalarSubquery.new(query)
  def self.expression(expression) = AssignmentValue.new(expression)
  def self.excluded(name: String) = ExcludedAttribute.new(name)
  def self.literal(value) = Literal.new(value)
  def self.cast(expression, type_name: String) = Cast.new(expression, type_name)
  def self.integer_operator(expression, operator: String, value)
    if operator != "&" && operator != "|" && operator != "<<" && operator != ">>"
      raise ArgumentError.new("unsupported SQL integer operator")
    end
    BinaryExpression.new(expression, operator, value)
  end
  def self.conflict_target(columns) = ConflictTarget.new(arel_array(columns))
  def self.conflict_target_on_constraint(name: String) = ConflictConstraintTarget.new(name)
  def self.column_default() = ColumnDefault.new()
  def self.render(statement, visitor = nil) = statement.to_sql(visitor)
  def self.inspect(node) = Inspector.new().inspect(node)
  def self.same?(left, right) = Inspector.new().same?(left, right)
  def self.children(node) = Inspector.new().children(node)
  def self.walk(node, visitor = nil) = Inspector.new().walk(node, visitor)
  def self.simplify(node, rules = [], report = false) = Inspector.new().simplify(node, rules, report)
  def self.with_children(node, replacements: Array) = Inspector.new().with_children(node, replacements)
  def self.union(left, right) = CompoundQuery.new(left, "UNION", right)
  def self.union_all(left, right) = CompoundQuery.new(left, "UNION ALL", right)
  def self.intersect(left, right) = CompoundQuery.new(left, "INTERSECT", right)
  def self.except(left, right) = CompoundQuery.new(left, "EXCEPT", right)
  def self.insert_into(table: Arel::Table) = Insert.new(table)
  def self.update(table: Arel::Table) = Update.new(table)
  def self.delete_from(table: Arel::Table) = Delete.new(table)
  def self.from_subquery(query, name: String)
    Query.new(name, [], [], nil, nil, [RawSql.new("*", [])], true, true,
      name, false, [], [], [], query)
  end
  def self.from(table)
    if table is Table
      Query.for_table(table)
    else
      Query.new(table, [], [], nil, nil, ["*"], false, false)
    end
  end

end
