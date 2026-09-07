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
      associations = mapping.associations()
      lookahead.selections().lazy().map() do |name|
        associations[name]
      end.reject() do |association|
        association == nil
      end.force()
    end

    def nested_associations(mapping, lookahead)
      selected = self.selected_associations(mapping, lookahead)
      nested = selected.lazy().map() do |association|
        reflection = mapping.repository().reflect_on_association(association.name())
        target = association.target()
        if reflection != nil && target != nil && !reflection.polymorphic?() &&
           reflection.through_reflection() == nil &&
           target.repository().table().name() == reflection.target_repository().table().name()
          NestedAssociation.new(
            association, reflection, target, lookahead.selection(association.field_name()))
        else
          nil
        end
      end.reject() do |association|
        association == nil
      end.force()
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
      names = lookahead.selections()
      column_mappings = mapping.columns()
      mapped = names.lazy().map() do |name|
        column = column_mappings[name]
        unless column == nil
          unless repository.has_column?(column)
            raise UnknownColumnError.for_mapped_column(
              mapping.type_name(), name, column, repository.table().name())
          end
        end
        column
      end.reject() do |column|
        column == nil
      end.force()
      required.each() do |column|
        unless repository.has_column?(column)
          raise UnknownColumnError.for_required_column(column, repository.table().name())
        end
      end

      selected = self.selected_associations(mapping, lookahead)
      association_names = selected.map() do |association| association.name() end
      association_names = association_names.concat(required_associations)
      foreign_keys = association_names.lazy().map() do |association_name|
        reflection = repository.reflect_on_association(association_name)
        if reflection != nil && reflection.belongs_to?() && !reflection.polymorphic?()
          reflection.foreign_key()
        else
          nil
        end
      end.reject() do |foreign_key|
        foreign_key == nil
      end.force()

      columns = [repository.primary_key()]
      inheritance = repository.inheritance_column()
      if inheritance != nil && repository.has_column?(inheritance)
        columns.push(inheritance)
      end
      columns = columns.concat(mapped).concat(foreign_keys).concat(required)
      unique = columns.map() do |column| "#{column}" end.uniq()
      expressions = unique.map() do |column| repository.table().column(column) end
      relation.select(expressions)
    end

    def resolve_relation(relation, mapping, lookahead, required_associations, required_columns)
      selected = self.select_columns(
        relation, mapping, lookahead, required_columns, required_associations)
      nested = self.nested_associations(mapping, lookahead)
      nested_names = nested.map() do |association| association.mapping().name() end
      associations = self.selected_associations(mapping, lookahead)
      eager_names = associations.map() do |association| association.name() end
      required_names = required_associations.map() do |name| "#{name}" end
      eager_names = eager_names.concat(required_names)
      eager_names = eager_names.reject() do |name| nested_names.include?(name) end
      eager_names = eager_names.uniq()

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
      nested.each() do |association|
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
        records.each() do |record|
          value = record.preloaded_association(association.mapping().name())
          if value is Array
            children = children.concat(value)
          elsif value != nil
            children.push(value)
          end
        end
        child_nested = self.nested_associations(
          association.target_mapping(), association.lookahead())
        self.preload_nested(children, child_nested)
      end
    end

    private(selected_associations, nested_associations, validate_no_aliased_duplicates,
      select_columns, resolve_relation, preload_nested)
  end

end
