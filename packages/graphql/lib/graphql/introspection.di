# __schema/__type(name:)/__typename -- the spec's introspection system,
# needed for basically any GraphQL client tooling (GraphiQL, Apollo,
# etc.) to introspect a server at all. `__typename` is already handled
# generically inside execution/executor.di's own #execute_field (valid
# on any composite-typed selection, not just the query root); this file
# builds the __Schema/__Type/__Field/__InputValue/__EnumValue/
# __Directive meta-object-types that `__schema`/`__type` resolve
# through, using the exact same GraphQL::ObjectType builder API a
# user's own schema uses -- no separate introspection-specific
# machinery, just ordinary types whose "object" happens to be one of
# this package's own GraphQL::Type/Field/Argument/Schema values, and
# whose resolvers call straight through to those values' own
# already-existing methods.
#
# Not ported: description tracking on types themselves (Field/Argument
# already carry a description; ObjectType/InterfaceType/UnionType/
# EnumType/InputObjectType/ScalarType don't yet -- #type_description
# below always returns nil, a documented v1 gap, see ROADMAP.md),
# deprecation (isDeprecated/deprecationReason always false/nil -- no
# deprecation-marking mechanism exists anywhere in this package yet),
# and a real directive registry (@include/@skip/@deprecated are handled
# structurally by the executor but were never modeled as introspectable
# GraphQL::Directive values, so __Schema.directives always returns an
# empty list).
module GraphQL

  # Every resolver here is a plain `self.` method, never nested inside
  # another -- deliberately, to avoid the nested-def-inside-a-self.-
  # method arity bug execution/coercion.di's own header comment
  # documents. None of these capture anything from an enclosing scope
  # (everything they need arrives via `object`/`args`), so there was
  # never a reason to nest them in the first place.
  class IntrospectionResolvers
    def self.type_kind(object, args, context) = object.kind()
    def self.type_name(object, args, context) = object.name()
    def self.type_description(object, args, context) = nil

    def self.type_fields(object, args, context)
      if object.kind() == "OBJECT" || object.kind() == "INTERFACE"
        object.fields()
      else
        nil
      end
    end

    def self.type_interfaces(object, args, context)
      if object.kind() == "OBJECT"
        object.interfaces()
      else
        nil
      end
    end

    def self.type_possible_types(object, args, context)
      if object.kind() == "UNION"
        object.possible_types()
      elsif object.kind() == "INTERFACE"
        object.implementors()
      else
        nil
      end
    end

    def self.type_enum_values(object, args, context)
      if object.kind() == "ENUM"
        object.values()
      else
        nil
      end
    end

    def self.type_input_fields(object, args, context)
      if object.kind() == "INPUT_OBJECT"
        object.arguments()
      else
        nil
      end
    end

    def self.type_of_type(object, args, context)
      if object.kind() == "LIST" || object.kind() == "NON_NULL"
        object.of_type()
      else
        nil
      end
    end

    def self.field_name(object, args, context) = object.name()
    def self.field_description(object, args, context) = object.description()
    def self.field_arguments(object, args, context) = object.arguments()
    def self.field_type(object, args, context) = object.type()
    def self.field_is_deprecated(object, args, context) = false
    def self.field_deprecation_reason(object, args, context) = nil

    def self.input_value_name(object, args, context) = object.name()
    def self.input_value_description(object, args, context) = object.description()
    def self.input_value_type(object, args, context) = object.type()

    def self.input_value_default_value(object, args, context)
      if object.has_default?()
        self.print_literal(object.default_value())
      else
        nil
      end
    end

    # Renders a plain coerced Diamond value (an Argument/input-field's
    # own #default_value, never an AST node -- see argument.di) back into
    # GraphQL literal syntax, for __InputValue.defaultValue's own
    # `String` shape. Deliberately simple: doesn't escape special
    # characters inside a String value, since no default value anywhere
    # in this package's own test suite needs one -- a real gap, not
    # invisible, just not worth a full escaping implementation for a
    # field most clients only display for documentation purposes.
    def self.print_literal(value)
      if value == nil
        "null"
      elsif value is String
        "\"#{value}\""
      elsif value is Array
        parts = []
        index = 0
        while index < value.length()
          parts.push(self.print_literal(value[index]))
          index += 1
        end
        "[#{parts.join(", ")}]"
      elsif value is Hash
        parts = []
        keys = value.keys()
        index = 0
        while index < keys.length()
          parts.push("#{keys[index]}: #{self.print_literal(value[keys[index]])}")
          index += 1
        end
        "{#{parts.join(", ")}}"
      else
        "#{value}"
      end
    end

    # `object` is a plain {"name":, "description":} Hash here, matching
    # EnumType#values' own convention (enum_type.di).
    def self.enum_value_name(object, args, context) = object["name"]
    def self.enum_value_description(object, args, context) = object["description"]
    def self.enum_value_is_deprecated(object, args, context) = false
    def self.enum_value_deprecation_reason(object, args, context) = nil

    # Dead code for v1 in practice -- #schema_directives always returns
    # an empty list (no directive registry exists), so nothing ever
    # resolves a __Directive value through these. Kept only so
    # __Directive's own field list is complete/spec-shaped if a future
    # version adds real directive registration.
    def self.directive_locations(object, args, context) = []

    def self.schema_types(object, args, context) = object.type_map().values()
    def self.schema_query_type(object, args, context) = object.query_type()
    def self.schema_mutation_type(object, args, context) = object.mutation_type()
    def self.schema_subscription_type(object, args, context) = nil
    def self.schema_directives(object, args, context) = []
  end

  class Introspection
    # Builds the six meta-object-types fresh, wired for their own
    # mutual/self references (`__Type.ofType` -> `__Type`, `__Field.type`
    # -> `__Type`, ...) -- ordinary Diamond values with ordinary variable
    # references, not a compile-time class-declaration cycle, so none of
    # type.di's own same-file-forward-declaration caveats apply here:
    # `type_meta.field("ofType", type_meta, ...)` just needs `type_meta`
    # to already be a value (from `ObjectType.new` above it), not a
    # finished/fully-built one.
    #
    # Returns {"__Schema": ..., "__Type": ...} -- the two Executor
    # actually needs directly (for the `__schema`/`__type` root fields);
    # every other meta-type is only ever reached *through* one of those
    # two's own field chain, so nothing else needs to be handed back.
    def self.build_meta_types()
      type_meta = GraphQL::ObjectType.new("__Type")
      field_meta = GraphQL::ObjectType.new("__Field")
      input_value_meta = GraphQL::ObjectType.new("__InputValue")
      enum_value_meta = GraphQL::ObjectType.new("__EnumValue")
      directive_meta = GraphQL::ObjectType.new("__Directive")
      schema_meta = GraphQL::ObjectType.new("__Schema")

      type_kind_enum = GraphQL::EnumType.new("__TypeKind")
      type_kind_enum.value("SCALAR")
      type_kind_enum.value("OBJECT")
      type_kind_enum.value("INTERFACE")
      type_kind_enum.value("UNION")
      type_kind_enum.value("ENUM")
      type_kind_enum.value("INPUT_OBJECT")
      type_kind_enum.value("LIST")
      type_kind_enum.value("NON_NULL")

      type_meta.field("kind", type_kind_enum.non_null(), IntrospectionResolvers.type_kind)
      type_meta.field("name", GraphQL::ScalarType.string(), IntrospectionResolvers.type_name)
      type_meta.field("description", GraphQL::ScalarType.string(), IntrospectionResolvers.type_description)
      type_meta.field("fields", field_meta.list(), IntrospectionResolvers.type_fields)
      type_meta.field("interfaces", type_meta.list(), IntrospectionResolvers.type_interfaces)
      type_meta.field("possibleTypes", type_meta.list(), IntrospectionResolvers.type_possible_types)
      type_meta.field("enumValues", enum_value_meta.list(), IntrospectionResolvers.type_enum_values)
      type_meta.field("inputFields", input_value_meta.list(), IntrospectionResolvers.type_input_fields)
      type_meta.field("ofType", type_meta, IntrospectionResolvers.type_of_type)

      field_meta.field("name", GraphQL::ScalarType.string().non_null(), IntrospectionResolvers.field_name)
      field_meta.field("description", GraphQL::ScalarType.string(), IntrospectionResolvers.field_description)
      field_meta.field("args", input_value_meta.list().non_null(), IntrospectionResolvers.field_arguments)
      field_meta.field("type", type_meta.non_null(), IntrospectionResolvers.field_type)
      field_meta.field("isDeprecated", GraphQL::ScalarType.boolean().non_null(), IntrospectionResolvers.field_is_deprecated)
      field_meta.field("deprecationReason", GraphQL::ScalarType.string(), IntrospectionResolvers.field_deprecation_reason)

      input_value_meta.field("name", GraphQL::ScalarType.string().non_null(), IntrospectionResolvers.input_value_name)
      input_value_meta.field("description", GraphQL::ScalarType.string(), IntrospectionResolvers.input_value_description)
      input_value_meta.field("type", type_meta.non_null(), IntrospectionResolvers.input_value_type)
      input_value_meta.field("defaultValue", GraphQL::ScalarType.string(), IntrospectionResolvers.input_value_default_value)

      enum_value_meta.field("name", GraphQL::ScalarType.string().non_null(), IntrospectionResolvers.enum_value_name)
      enum_value_meta.field("description", GraphQL::ScalarType.string(), IntrospectionResolvers.enum_value_description)
      enum_value_meta.field("isDeprecated", GraphQL::ScalarType.boolean().non_null(), IntrospectionResolvers.enum_value_is_deprecated)
      enum_value_meta.field("deprecationReason", GraphQL::ScalarType.string(), IntrospectionResolvers.enum_value_deprecation_reason)

      directive_meta.field("name", GraphQL::ScalarType.string().non_null(), IntrospectionResolvers.field_name)
      directive_meta.field("description", GraphQL::ScalarType.string(), IntrospectionResolvers.field_description)
      directive_meta.field("locations", GraphQL::ScalarType.string().non_null().list().non_null(), IntrospectionResolvers.directive_locations)
      directive_meta.field("args", input_value_meta.list().non_null(), IntrospectionResolvers.field_arguments)

      schema_meta.field("types", type_meta.non_null().list().non_null(), IntrospectionResolvers.schema_types)
      schema_meta.field("queryType", type_meta.non_null(), IntrospectionResolvers.schema_query_type)
      schema_meta.field("mutationType", type_meta, IntrospectionResolvers.schema_mutation_type)
      schema_meta.field("subscriptionType", type_meta, IntrospectionResolvers.schema_subscription_type)
      schema_meta.field("directives", directive_meta.non_null().list().non_null(), IntrospectionResolvers.schema_directives)

      {"__Schema": schema_meta, "__Type": type_meta}
    end
  end

end
