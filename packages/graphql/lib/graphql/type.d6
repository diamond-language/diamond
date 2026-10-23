module GraphQL

  # Common base every concrete type kind (ScalarType, ObjectType,
  # InterfaceType, UnionType, EnumType, InputObjectType, and the two
  # wrapper kinds, ListType and NonNullType) subclasses, purely for these two chainable
  # wrapping methods -- there is no shared behavior beyond this. Diamond
  # has no class-body macros (see ROADMAP.md), so every concrete type is
  # built as a plain value via chained instance-method calls
  # (packages/dials's Router#get/#post is the template), not a DSL.
  class Type
    def non_null() = NonNullType.new(self)
    def list() = ListType.new(self)
  end

end
