module GraphSQL

class Resolver
  def initialize(relation, db, lookahead, mapping: Mapping,
                 required_associations: Array = [], required_columns: Array = [])
    @relation = relation
    @db = db
    @lookahead = lookahead
    @mapping = mapping
    @required_associations = required_associations
    @required_columns = required_columns
  end

  def resolve()
    unless @relation is ActiveRecord::Relation
      return @relation
    end
    unless @relation.repository() == @mapping.repository()
      return @relation
    end
    self.resolve_relation(@relation, @mapping, @lookahead,
      @required_associations, @required_columns)
  end

  def selected_associations(mapping, lookahead)
    selected = []
    names = lookahead.selections()
    associations = mapping.associations()
    index = 0
    while index < names.length()
      association = associations[names[index]]
      unless association == nil
        selected.push(association)
      end
      index += 1
    end
    selected
  end

  def nested_associations(mapping, lookahead)
    selected = self.selected_associations(mapping, lookahead)
    nested = []
    index = 0
    while index < selected.length()
      association = selected[index]
      reflection = mapping.repository().reflect_on_association(association.name())
      target = association.target()
      if reflection != nil && target != nil && !reflection.polymorphic?() &&
         reflection.through_reflection() == nil &&
         target.repository().table().name() == reflection.target_repository().table().name()
        nested.push(NestedAssociation.new(
          association, reflection, target, lookahead.selection(association.field_name())))
      end
      index += 1
    end
    self.validate_no_aliased_duplicates(mapping, nested)
    nested
  end

  def validate_no_aliased_duplicates(mapping, nested)
    index = 0
    while index < nested.length()
      fields = [nested[index].mapping().field_name()]
      other_index = index + 1
      while other_index < nested.length()
        if nested[other_index].reflection().name() == nested[index].reflection().name()
          fields.push(nested[other_index].mapping().field_name())
        end
        other_index += 1
      end
      if fields.length() > 1
        raise AliasedAssociationError.for_duplicate(
          mapping.type_name(), nested[index].reflection().name(), fields)
      end
      index += 1
    end
  end

  def select_columns(relation, mapping, lookahead, required: Array,
                     required_associations: Array = [])
    repository = mapping.repository()
    mapped = []
    names = lookahead.selections()
    column_mappings = mapping.columns()
    index = 0
    while index < names.length()
      column = column_mappings[names[index]]
      unless column == nil
        unless repository.has_column?(column)
          raise UnknownColumnError.for_mapped_column(
            mapping.type_name(), names[index], column, repository.table().name())
        end
        mapped.push(column)
      end
      index += 1
    end
    index = 0
    while index < required.length()
      unless repository.has_column?(required[index])
        raise UnknownColumnError.for_required_column(required[index], repository.table().name())
      end
      index += 1
    end

    foreign_keys = []
    selected = self.selected_associations(mapping, lookahead)
    all_associations = selected
    index = 0
    while index < all_associations.length()
      reflection = repository.reflect_on_association(all_associations[index].name())
      if reflection != nil && reflection.belongs_to?() && !reflection.polymorphic?()
        foreign_keys.push(reflection.foreign_key())
      end
      index += 1
    end
    index = 0
    while index < required_associations.length()
      reflection = repository.reflect_on_association(required_associations[index])
      if reflection != nil && reflection.belongs_to?() && !reflection.polymorphic?()
        foreign_keys.push(reflection.foreign_key())
      end
      index += 1
    end

    columns = [repository.primary_key()]
    inheritance = repository.inheritance_column()
    if inheritance != nil && repository.has_column?(inheritance)
      columns.push(inheritance)
    end
    columns = columns.concat(mapped).concat(foreign_keys).concat(required)
    unique = []
    index = 0
    while index < columns.length()
      name = "#{columns[index]}"
      unless unique.include?(name)
        unique.push(name)
      end
      index += 1
    end
    expressions = []
    index = 0
    while index < unique.length()
      expressions.push(repository.table().column(unique[index]))
      index += 1
    end
    relation.select(expressions)
  end

  def resolve_relation(relation, mapping, lookahead, required_associations, required_columns)
    selected = self.select_columns(
      relation, mapping, lookahead, required_columns, required_associations)
    nested = self.nested_associations(mapping, lookahead)
    nested_names = []
    index = 0
    while index < nested.length()
      nested_names.push(nested[index].mapping().name())
      index += 1
    end
    eager_names = []
    associations = self.selected_associations(mapping, lookahead)
    index = 0
    while index < associations.length()
      name = associations[index].name()
      unless nested_names.include?(name) || eager_names.include?(name)
        eager_names.push(name)
      end
      index += 1
    end
    index = 0
    while index < required_associations.length()
      name = "#{required_associations[index]}"
      unless eager_names.include?(name) || nested_names.include?(name)
        eager_names.push(name)
      end
      index += 1
    end

    if nested.length() == 0
      if eager_names.length() == 0 then selected else selected.includes(eager_names) end
    else
      records = selected.to_a(@db)
      if eager_names.length() > 0
        ActiveRecord::Associations::Preloader.new(records, eager_names).call(@db)
      end
      self.preload_nested(records, nested)
      records
    end
  end

  def preload_nested(records, nested)
    index = 0
    while index < nested.length()
      association = nested[index]
      required = if association.reflection().belongs_to?()
        []
      else
        [association.reflection().foreign_key()]
      end
      scope = self.select_columns(
        association.target_mapping().repository().relation(),
        association.target_mapping(), association.lookahead(), required)
      ActiveRecord::Associations::Preloader.new(
        records, association.mapping().name(), scope).call(@db)

      children = []
      record_index = 0
      while record_index < records.length()
        value = records[record_index].preloaded_association(association.mapping().name())
        if value is Array
          children = children.concat(value)
        elsif value != nil
          children.push(value)
        end
        record_index += 1
      end
      child_nested = self.nested_associations(
        association.target_mapping(), association.lookahead())
      self.preload_nested(children, child_nested)
      index += 1
    end
  end
end

end
