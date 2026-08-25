module GraphQL

# `resolve` is a required Callable[3] -- `(object, args, context)` --
# never optional/auto-wired by matching method name. Diamond has no
# `send`/dispatch-by-runtime-string, and a GraphQL field's name only
# exists as a runtime string parsed out of the query document, so
# there's no way to look up "the method named by this string" the way
# graphql-ruby's own default resolution (`obj.public_send(field.method_sym)`)
# does. Every field's resolver is supplied explicitly at registration
# time -- typically a bare `self.` singleton method reference
# (`SomeModule.resolve_thing`, a zero-capture Callable value) or an
# inline closure.
class Field
  attr_reader name
  attr_reader type
  attr_reader resolve
  attr_reader arguments
  attr_reader description

  def initialize(name, type, resolve, arguments = [], description = nil)
    @name = name
    @type = type
    @resolve = resolve
    @arguments = arguments
    @description = description
  end

  # nil if this field has no argument by that name.
  def argument(arg_name)
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
