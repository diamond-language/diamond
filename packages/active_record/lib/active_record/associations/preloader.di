module ActiveRecord
module Associations

# Rails-shaped entry point for attaching one reflected association to a set of
# already-loaded models. An optional target Relation supplies projections,
# filters, ordering, and nested includes; the reflection still adds the batched
# owner-key predicate itself.
class Preloader
  attr_reader records: Array
  attr_reader association: String
  attr_reader scope

  def initialize(records: Array, association, scope = nil)
    @records = records
    @association = "#{association}"
    @scope = scope
  end

  def call(db)
    if @records.length() == 0
      return @records
    end
    repository = @records[0].repository()
    reflection = repository.reflect_on_association(@association)
    if reflection == nil
      raise AssociationNotFoundError.new(@association)
    end
    reflection.preload(db, @records, @scope)
  end
end

end
end
