module GraphQL

  class UnionType < Type
    def initialize(name)
      @union_name = name
      @possible_types = []
      @type_resolver = nil
    end

    def name() = @union_name
    def kind() = "UNION"

    def possible_type(object_type)
      @possible_types << object_type
      self
    end

    def possible_types() = @possible_types

    # `callable` is `(object, context) -> String` -- the concrete member
    # ObjectType's name for a resolved value. Required for any
    # union-typed field the executor actually completes, same reasoning
    # as InterfaceType#resolve_type.
    def resolve_type(callable)
      @type_resolver = callable
      self
    end

    def type_resolver() = @type_resolver

    def includes?(object_type_name)
      @possible_types.find() do |possible_type|
        possible_type.name() == object_type_name
      end != nil
    end
  end

end
