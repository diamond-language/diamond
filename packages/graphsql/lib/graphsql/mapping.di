module GraphSQL

# Explicit Diamond counterpart to graphsql-ruby's type-class DSL. GraphQL
# object types are runtime values here rather than subclass declarations, so a
# Mapping is a separate fluent value pairing a type name with its repository.
class Mapping
  attr_reader repository: ActiveRecord::Repository
  attr_reader type_name: String
  attr_reader parent

  def initialize(repository: ActiveRecord::Repository, type_name: String, parent = nil)
    @repository = repository
    @type_name = type_name
    @parent = parent
    @columns = {}
    @associations = {}
  end

  def column(name, field_name = nil)
    field = if field_name == nil then "#{name}" else "#{field_name}" end
    @columns[field] = "#{name}"
    self
  end

  def association(name, field_name = nil, target = nil)
    field = if field_name == nil then "#{name}" else "#{field_name}" end
    @associations[field] = AssociationMapping.new(name, field, target)
    self
  end

  def columns()
    inherited = if @parent == nil then {} else @parent.columns() end
    inherited.merge(@columns)
  end

  def associations()
    inherited = if @parent == nil then {} else @parent.associations() end
    inherited.merge(@associations)
  end
end

end
