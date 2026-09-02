module GraphQL

  # `GraphQL::ObjectType.new("Author").field("id", ID.non_null(), resolver)
  # .field("name", STRING, resolver2)` -- a fluent builder over an
  # internal Array, `Router#get`/`#post`'s own shape
  # (packages/dials/lib/dials/router.di), not a class-body macro (Diamond
  # has none).
  class ObjectType < Type
    def initialize(name)
      @object_name = name
      @fields = []
      @interfaces = []
    end

    def name() = @object_name
    def kind() = "OBJECT"

    def field(name, type, resolve, arguments = [], description = nil)
      @fields << Field.new(name, type, resolve, arguments, description)
      self
    end

    def implements(interface_type)
      @interfaces << interface_type
      interface_type.register_implementor(self)
      self
    end

    def fields() = @fields
    def interfaces() = @interfaces

    # nil if this type has no field by that name -- the executor's own
    # field-lookup-by-selection-name uses this.
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
