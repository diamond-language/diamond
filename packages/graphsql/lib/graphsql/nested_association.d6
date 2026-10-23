module GraphSQL

  class NestedAssociation
    attr_reader mapping: AssociationMapping
    attr_reader reflection: ActiveRecord::AssociationReflection
    attr_reader target_mapping: Mapping
    attr_reader lookahead

    def initialize(mapping, reflection, target_mapping, lookahead)
      @mapping = mapping
      @reflection = reflection
      @target_mapping = target_mapping
      @lookahead = lookahead
    end
  end

end
