module ActiveRecord
  module Associations

    # Rails-shaped entry point for attaching one reflected association to a set of
    # already-loaded models. An optional target Relation supplies projections,
    # filters, ordering, and nested includes; the reflection still adds the batched
    # owner-key predicate itself.
    class Preloader
      attr_reader records: Array
      attr_reader associations: Array
      attr_reader scope

      def initialize(records: Array, associations, scope = nil)
        @records = records
        values = if associations is Array then associations else [associations] end
        @associations = []
        index = 0
        while index < values.length()
          name = "#{values[index]}"
          unless @associations.include?(name)
            @associations.push(name)
          end
          index += 1
        end
        @scope = scope
      end

      def call(db)
        if @records.length() == 0
          return @records
        end
        repository = @records[0].repository()
        index = 0
        while index < @associations.length()
          name = @associations[index]
          reflection = repository.reflect_on_association(name)
          if reflection == nil
            raise AssociationNotFoundError.new(name)
          end
          reflection.preload(db, @records, @scope)
          index += 1
        end
        @records
      end
    end

  end
end
