module GraphSQL

class AliasedAssociationError < StandardError
  def self.for_duplicate(type_name, association, fields)
    joined_fields = fields.join(", ")
    AliasedAssociationError.new(
      "#{type_name} maps association '#{association}' under multiple requested GraphQL fields " +
      "(#{joined_fields})")
  end
end

end
