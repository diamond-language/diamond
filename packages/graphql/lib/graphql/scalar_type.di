module GraphQL

# `coerce_input` takes a raw value (already lexed/parsed out of a query
# document literal, or out of a variables Hash) and returns the
# internal Diamond value to hand resolvers -- e.g. Int's coerce_input
# just returns the Int unchanged (the lexer already produced a real
# Int), while a hypothetical custom scalar might parse a String into
# something richer. `coerce_result` takes a resolver's return value and
# returns the value to place in the JSON-shaped result -- for the
# built-ins below this is always identity, but a custom scalar (a Time
# serialized as an ISO8601 String, say) would differ in this direction
# too. Both are required Callable[1]s -- no default/identity fallback,
# so a scalar's coercion behavior is always explicit at the definition
# site, matching this package's own no-auto-wiring stance on Field's
# resolve.
class ScalarType < Type
  attr_reader coerce_input
  attr_reader coerce_result

  def initialize(name, coerce_input, coerce_result)
    @scalar_name = name
    @coerce_input = coerce_input
    @coerce_result = coerce_result
  end

  def name() = @scalar_name
  def kind() = "SCALAR"

  # The five built-in scalars, one factory method each rather than a
  # true module-level constant -- Diamond has no top-level constant
  # binding (confirmed: nothing in docs/syntax.md describes one, and
  # every "constant-like" value in this codebase, e.g.
  # packages/http/lib/http/status.di's http_max_body_size(), is a
  # function returning a fresh value per call). Each call below
  # allocates a new ScalarType instance; that's fine, these are
  # immutable value objects with no identity semantics anything in this
  # package depends on.
  def self.identity(value) = value

  def self.string() = ScalarType.new("String", ScalarType.identity, ScalarType.identity)
  def self.int() = ScalarType.new("Int", ScalarType.identity, ScalarType.identity)
  def self.float() = ScalarType.new("Float", ScalarType.identity, ScalarType.identity)
  def self.boolean() = ScalarType.new("Boolean", ScalarType.identity, ScalarType.identity)
  # ID coerces exactly like String (both accept String or Int literals
  # in real GraphQL; this v1 keeps it simple and treats an ID's
  # incoming value as already-correct, same as String/Int) -- it's a
  # distinct type name for introspection/schema purposes only.
  def self.id() = ScalarType.new("ID", ScalarType.identity, ScalarType.identity)
end

end
