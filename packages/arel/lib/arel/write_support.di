module Arel

class AssignmentValue
  def initialize(expression)
    @expression = expression
  end
  def expression() = @expression
end

class ConflictTarget
  def initialize(columns: Array, predicate = nil)
    @columns = columns
    @predicate = predicate
  end
  def columns() = @columns
  def predicate() = @predicate
  def where(predicate) = ConflictTarget.new(@columns, predicate)
  def column(name: String) = ConflictAttribute.new(name)
end

# `ON CONFLICT ON CONSTRAINT name DO ...` -- names the constraint directly
# instead of repeating its column list (or for a constraint ConflictTarget's
# column-list form can't express at all, like an exclusion constraint).
# Verified directly: PostgreSQL accepts this; SQLite has no equivalent
# (rejects it as a syntax error -- SQLite's own UPSERT grammar has no
# named-constraint form), so this is gated behind the "named-constraint
# conflict targets" capability.
class ConflictConstraintTarget
  def initialize(name: String)
    @name = name
  end
  def name() = @name
end

class DefaultValues
end

# A per-column DEFAULT within an ordinary VALUES row (`INSERT INTO t (a,
# b) VALUES (1, DEFAULT)`), as opposed to DefaultValues above (the whole
# row is DEFAULT VALUES, no column list at all). Verified directly against
# both dialects before adding this: PostgreSQL accepts a bare DEFAULT in a
# VALUES list (single- and multi-row); SQLite rejects it outright as a
# syntax error, so this is gated behind the "per-column default values"
# capability, unlike Cast above.
class ColumnDefault
end

# Namespace singleton methods rather than free top-level functions:
# Diamond's top-level function resolution is source-order (a call only
# sees functions already defined earlier in the file), and these need
# Cte/ConflictTarget/AssignmentValue already declared, but are also
# called from Insert/Update/Delete below -- nesting them here, after the
# classes they use and before their own call sites, and calling them
# self-referentially as Arel.append_cte(...)/Arel.render_insert_conflict(...)
# (verified this resolves the same way an external Arel.table(...) call
# does), satisfies both constraints at once.
def self.append_cte(ctes: Array, name: String, query, recursive = false) -> Array
  duplicate = false
  index = 0
  while index < ctes.length()
    if ctes[index].name().downcase() == name.downcase()
      duplicate = true
    end
    index += 1
  end
  if duplicate
    raise ArgumentError.new("duplicate CTE name")
  end
  ctes.concat([Cte.new(name, query, recursive)])
end

def self.render_insert_conflict(target, ignore: Bool, assignments, params: Array,
                                visitor) -> String
  if !ignore && assignments == nil
    return ""
  end
  target_sql = ""
  if target is ConflictConstraintTarget
    visitor.require_extension("named-constraint conflict targets")
    target_sql = " ON CONSTRAINT #{visitor.quote_identifier(target.name())}"
  else
    columns = target
    predicate = nil
    if target is ConflictTarget
      columns = target.columns()
      predicate = target.predicate()
    end
    targets = []
    target_index = 0
    while target_index < columns.length()
      targets.push(visitor.quote_identifier(columns[target_index]))
      target_index += 1
    end
    if targets.length() > 0
      target_sql = " (#{targets.join(", ")})"
    end
    unless predicate == nil
      visitor.require_extension("conflict-target predicates")
      target_sql += " WHERE " + visitor.render_expression(predicate, params)
    end
  end
  visitor.require_extension("upsert conflict actions")
  if ignore
    return " ON CONFLICT#{target_sql} DO NOTHING"
  end
  if assignments.length() == 0
    raise ArgumentError.new("conflict update requires at least one assignment")
  end
  rendered_assignments = []
  assignment_index = 0
  while assignment_index < assignments.length()
    name = assignments.key_at(assignment_index)
    value = assignments[name]
    if value is AssignmentValue
      rendered = visitor.render_expression(value.expression(), params)
      rendered_assignments.push("#{visitor.quote_identifier(name)} = #{rendered}")
    else
      rendered_assignments.push("#{visitor.quote_identifier(name)} = ?")
      params.push(value)
    end
    assignment_index += 1
  end
  " ON CONFLICT#{target_sql} DO UPDATE SET #{rendered_assignments.join(", ")}"
end

end
