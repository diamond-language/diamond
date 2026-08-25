module GraphQL

# Each value is a plain Hash ({"name": ..., "description": ...}), not a
# dedicated EnumValue class -- matches packages/dials/lib/dials/router.di's
# own convention of using a Hash for a small internal metadata record
# rather than inventing a class for it.
class EnumType < Type
  def initialize(name)
    @enum_name = name
    @values = []
  end

  def name() = @enum_name
  def kind() = "ENUM"

  def value(value_name, description = nil)
    @values << {"name": value_name, "description": description}
    self
  end

  def values() = @values

  def value_named?(value_name)
    index = 0
    found = false
    while index < @values.length() && !found
      if @values[index]["name"] == value_name
        found = true
      end
      index += 1
    end
    found
  end
end

end
