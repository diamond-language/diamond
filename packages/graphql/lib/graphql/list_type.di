module GraphQL

  # Wraps `of_type` -- e.g. `GraphQL::ListType.of(STRING_TYPE)` renders as
  # `[String]`. Subclasses Type so `.non_null()`/`.list()` compose (a
  # `[String!]!` is `STRING.non_null().list().non_null()`).
  class ListType < Type
    attr_reader of_type

    def initialize(of_type)
      @of_type = of_type
    end

    def self.of(of_type) = GraphQL::ListType.new(of_type)

    def name() = "[#{@of_type.name()}]"
    def kind() = "LIST"
  end

end
