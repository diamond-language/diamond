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
  # because a scalar's input and result semantics should remain explicit at
  # the definition site.
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

    def self.describe(value)
      if value is String then "a String"
      elsif value is Int then "an Int"
      elsif value is Float then "a Float"
      elsif value is Bool then "a Boolean"
      elsif value is Array then "a list"
      elsif value is Hash then "an object"
      else "an unsupported value"
      end
    end

    # Input: a built-in scalar takes only its own kind of value, so a client cannot hand a resolver a list or an object
    # (`{"$ne": null}`) where it expects an Int or a String.
    def self.string_input(value)
      raise GraphQL::RequestError.new("expected a String, got #{ScalarType.describe(value)}") unless value is String
      value
    end

    def self.int_input(value)
      raise GraphQL::RequestError.new("expected an Int, got #{ScalarType.describe(value)}") unless value is Int
      value
    end

    def self.float_input(value)
      raise GraphQL::RequestError.new("expected a Float, got #{ScalarType.describe(value)}") unless value is Float || value is Int
      value
    end

    def self.boolean_input(value)
      raise GraphQL::RequestError.new("expected a Boolean, got #{ScalarType.describe(value)}") unless value is Bool
      value
    end

    def self.id_input(value)
      raise GraphQL::RequestError.new("expected an ID (a String or an Int), got #{ScalarType.describe(value)}") unless value is String || value is Int
      value
    end

    # Output: a built-in scalar serialises a primitive. A resolver that returns a list, a Hash or a model object for one would otherwise
    # put the whole object in the response (a record with its password digest, say), so that is an error instead.
    def self.primitive_result(value)
      if value is Array || value is Hash || !(value is String || value is Int || value is Float || value is Bool || value is Symbol)
        raise GraphQL::ExecutionError.new("a resolver returned #{ScalarType.describe(value)} for a scalar field")
      end
      value
    end

    def self.string() = ScalarType.new("String", ScalarType.string_input, ScalarType.primitive_result)
    def self.int() = ScalarType.new("Int", ScalarType.int_input, ScalarType.primitive_result)
    def self.float() = ScalarType.new("Float", ScalarType.float_input, ScalarType.primitive_result)
    def self.boolean() = ScalarType.new("Boolean", ScalarType.boolean_input, ScalarType.primitive_result)
    # ID coerces exactly like String (both accept String or Int literals
    # in real GraphQL; this v1 keeps it simple and treats an ID's
    # incoming value as already-correct, same as String/Int) -- it's a
    # distinct type name for introspection/schema purposes only.
    def self.id() = ScalarType.new("ID", ScalarType.id_input, ScalarType.primitive_result)
  end

end
