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
    @type_resolver = nil
    @implementors = []
  end

  def name() = @interface_name
  def kind() = "INTERFACE"

  def field(name, type, arguments = [], description = nil)
    @fields << Field.new(name, type, nil, arguments, description)
    self
  end

  def fields() = @fields

  # Populated by ObjectType#implements, not called directly -- an
  # object type reachable *only* through implementing this interface
  # (never itself returned by any concrete field elsewhere) would
  # otherwise be invisible to Schema#type_map's own reachability walk,
  # which starts from the query/mutation roots and follows field return
  # types: an interface's own #fields declare a *contract*, not which
  # concrete types satisfy it, so nothing about walking `fields()` ever
  # turns up an implementor. Confirmed as a real bug via testing (an
  # Author type reachable only via a Node interface field went missing
  # from #type_map, breaking resolve_type entirely) before adding this.
  def register_implementor(object_type)
    @implementors << object_type
    self
  end

  def implementors() = @implementors

  # `callable` is `(object, context) -> String` -- the concrete
  # implementing ObjectType's name for a resolved value. Required for
  # any interface-typed field the executor actually completes (there's
  # no way to guess which implementing type a plain Diamond value
  # belongs to otherwise, same "explicit Callable, no magic" reasoning
  # as Field#resolve). Chainable, matching #field/#implements.
  def resolve_type(callable)
    @type_resolver = callable
    self
  end

  def type_resolver() = @type_resolver

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
