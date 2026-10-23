module GraphQL

  # Wraps `of_type` -- e.g. a NonNullType(STRING) renders as `String!`.
  # Subclasses Type so `.non_null()`/`.list()` compose. Double-wrapping a
  # NonNullType in another NonNullType (`String!!`) isn't valid GraphQL,
  # but this class doesn't guard against it -- callers building a type
  # graph by hand are trusted the same way every other package here
  # trusts its own callers (see docs/design.md's error-handling
  # philosophy); nothing in this package's execution/validation phases
  # constructs one on its own.
  class NonNullType < Type
    attr_reader of_type

    def initialize(of_type)
      @of_type = of_type
    end

    def name() = "#{@of_type.name()}!"
    def kind() = "NON_NULL"
  end

end
