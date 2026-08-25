module GraphQL

class UnionType < Type
  def initialize(name)
    @union_name = name
    @possible_types = []
  end

  def name() = @union_name
  def kind() = "UNION"

  def possible_type(object_type)
    @possible_types << object_type
    self
  end

  def possible_types() = @possible_types

  def includes?(object_type_name)
    index = 0
    found = false
    while index < @possible_types.length() && !found
      if @possible_types[index].name() == object_type_name
        found = true
      end
      index += 1
    end
    found
  end
end

end
