module Arel

  class CteRelation < Table
    def recursive_body(anchor, recursive_branch)
      if anchor.base_reference_name().downcase() == self.name().downcase()
        raise ArgumentError.new("recursive CTE anchor cannot reference itself")
      end
      if recursive_branch.base_reference_name().downcase() != self.name().downcase()
        raise ArgumentError.new("recursive branch must reference its CTE relation")
      end
      CompoundQuery.new(anchor, "UNION ALL", recursive_branch)
    end
  end

end
