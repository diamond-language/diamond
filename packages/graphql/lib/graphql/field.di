module GraphQL

  # `resolve`, when present, is a Callable[3] -- `(object, args, context)`.
  # A nil resolver asks the executor to call the field's own name as a public,
  # zero-argument method on the runtime object. Fields that need arguments,
  # context, name translation, or Hash lookup still provide an explicit
  # resolver.
  class Field
    attr_reader name
    attr_reader type
    attr_reader resolve
    attr_reader arguments
    attr_reader description

    def initialize(name, type, resolve = nil, arguments = [], description = nil)
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
