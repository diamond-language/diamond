module GraphQL

# `GraphQL::Schema.new().query(query_type).mutation(mutation_type)` --
# this phase is just the builder shell (query/mutation registration +
# the type-registry walk needed later for introspection); `.execute`
# lives in a later phase once the language/execution layers exist.
class Schema
  def initialize()
    @query_type = nil
    @mutation_type = nil
  end

  def query(type)
    @query_type = type
    self
  end

  def mutation(type)
    @mutation_type = type
    self
  end

  def query_type() = @query_type
  def mutation_type() = @mutation_type

  # The named type a List/NonNull wrapper ultimately refers to --
  # unwraps any number of wrapper layers.
  def self.unwrap(type)
    if type.kind() == "LIST" || type.kind() == "NON_NULL"
      self.unwrap(type.of_type())
    else
      type
    end
  end

  def self.visit_arguments(arguments, visited)
    index = 0
    while index < arguments.length()
      self.visit(arguments[index].type(), visited)
      index += 1
    end
    nil
  end

  def self.visit_fields(fields, visited)
    index = 0
    while index < fields.length()
      field = fields[index]
      self.visit(field.type(), visited)
      self.visit_arguments(field.arguments(), visited)
      index += 1
    end
    nil
  end

  # Registers `type` (unwrapped to its named form) into `visited` and
  # recurses into everything it references -- field return types, field
  # argument types, interface implementations, union member types,
  # input object field types. `visited` (keyed by type name) doubles as
  # the cycle guard: a type already registered is never re-walked, so a
  # cyclic graph (Author -> books -> Book -> author -> Author) still
  # terminates.
  def self.visit(type, visited)
    named = self.unwrap(type)
    name = named.name()
    if visited[name] != nil
      return nil
    end
    visited[name] = named
    kind = named.kind()
    if kind == "OBJECT"
      self.visit_fields(named.fields(), visited)
      interfaces = named.interfaces()
      index = 0
      while index < interfaces.length()
        self.visit(interfaces[index], visited)
        index += 1
      end
    elsif kind == "INTERFACE"
      self.visit_fields(named.fields(), visited)
      implementors = named.implementors()
      index = 0
      while index < implementors.length()
        self.visit(implementors[index], visited)
        index += 1
      end
    elsif kind == "UNION"
      possible = named.possible_types()
      index = 0
      while index < possible.length()
        self.visit(possible[index], visited)
        index += 1
      end
    elsif kind == "INPUT_OBJECT"
      self.visit_arguments(named.arguments(), visited)
    end
    nil
  end

  # Every type reachable from the query/mutation roots, keyed by name.
  def type_map()
    visited = {}
    unless @query_type == nil
      Schema.visit(@query_type, visited)
    end
    unless @mutation_type == nil
      Schema.visit(@mutation_type, visited)
    end
    visited
  end

  def types() = self.type_map()

  # `Schema.new().query(query_type).execute(query_string)` -- a fresh
  # `GraphQL::Execution::Executor` per call (see executor.di's own
  # header comment for why: no state to reset between requests, no
  # shared mutable state to worry about). Returns `{"data": ...}`, or
  # `{"data": ..., "errors": [...]}` when at least one field errored,
  # or just `{"errors": [...]}` (no `"data"` key at all) for a
  # request-level failure -- an unparseable document, an unknown
  # operation, a variable/argument that doesn't coerce -- per spec.
  def execute(query_string, raw_variables = {}, context = {}, root_value = nil, operation_name = nil)
    GraphQL::Execution::Executor.new(self).execute(query_string, raw_variables, context, root_value, operation_name)
  end
end

end
