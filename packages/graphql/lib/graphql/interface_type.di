module GraphQL

# Interface fields declare a name/type/arguments contract but are never
# resolved directly -- only a concrete ObjectType that `.implements()`
# this interface actually resolves a value, via its own same-named
# field's resolver. So `.field(...)` here has no `resolve` parameter at
# all (unlike ObjectType#field); internally each Field is still built
# with Field.new's full positional shape, just with `nil` passed for
# resolve.
class InterfaceType < Type
  def initialize(name)
    @interface_name = name
    @fields = []
  end

  def name() = @interface_name
  def kind() = "INTERFACE"

  def field(name, type, arguments = [], description = nil)
    @fields << Field.new(name, type, nil, arguments, description)
    self
  end

  def fields() = @fields

  def field_named(field_name)
    index = 0
    found = nil
    while index < @fields.length() && found == nil
      if @fields[index].name() == field_name
        found = @fields[index]
      end
      index += 1
    end
    found
  end
end

end
