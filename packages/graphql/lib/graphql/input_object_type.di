module GraphQL

# An input object's own fields are typed inputs with no resolver --
# exactly what Argument already models (name/type/default/description),
# so this reuses it directly rather than inventing a near-duplicate
# class.
class InputObjectType < Type
  def initialize(name)
    @input_name = name
    @arguments = []
  end

  def name() = @input_name
  def kind() = "INPUT_OBJECT"

  def argument(name, type, default_value = nil, has_default = false, description = nil)
    @arguments << Argument.new(name, type, default_value, has_default, description)
    self
  end

  def arguments() = @arguments

  def argument_named(arg_name)
    index = 0
    found = nil
    while index < @arguments.length() && found == nil
      if @arguments[index].name() == arg_name
        found = @arguments[index]
      end
      index += 1
    end
    found
  end
end

end
