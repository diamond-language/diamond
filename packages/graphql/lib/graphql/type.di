module GraphQL

# Type, ListType, and NonNullType live in one file, not the one-class-
# per-file split the rest of this package follows: Type's own
# non_null()/list() methods construct ListType/NonNullType instances,
# and those two classes subclass Type -- a genuine circular reference.
# Diamond's forward-declaration support (docs/roadmap.md's "Compiler
# representation") only reaches within a single source file (a
# `discovery_pass` over that one file); a class used before its own
# `require` in a *different* file is still an "undefined namespaced
# class" compile error, confirmed directly against this exact case
# before settling on one file. Splitting these three the usual way
# isn't possible without breaking one direction of that cycle.

# Common base every concrete type kind (ScalarType, ObjectType,
# InterfaceType, UnionType, EnumType, InputObjectType, and the two
# wrapper kinds below) subclasses, purely for these two chainable
# wrapping methods -- there is no shared behavior beyond this. Diamond
# has no class-body macros (see ROADMAP.md), so every concrete type is
# built as a plain value via chained instance-method calls
# (packages/dials's Router#get/#post is the template), not a DSL.
class Type
  # Unqualified names, not GraphQL::NonNullType/GraphQL::ListType --
  # Diamond's same-file forward-declaration pass (docs/roadmap.md)
  # resolves a bare class name regardless of order, but a namespaced
  # `Module::Class` path does not participate in that pass, confirmed
  # directly (M::Derived referenced before its own declaration is an
  # "undefined namespaced class" compile error even in the same file,
  # while a bare `Derived` in the same position works). Being inside
  # `module GraphQL ... end` already, the bare names resolve to the
  # same classes.
  def non_null() = NonNullType.new(self)
  def list() = ListType.new(self)
end

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
