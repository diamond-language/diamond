# Variable/argument/input-object coercion -- turning raw GraphQL query
# text (AST value nodes, GraphQL::Language::*) and raw runtime values
# (a caller's own `variables` Hash, already-decoded Diamond values, no
# AST involved) into the values a resolver actually receives. Two
# entry shapes (coerce_literal vs coerce_runtime_value) because a
# query's own literal argument values and a caller-supplied variables
# Hash are structurally different inputs (AST nodes vs plain Diamond
# Hash/Array/Int/...), even though both ultimately coerce against the
# exact same GraphQL::Type shapes.
#
# A class with `self.` methods, not a `module_function` module
# (packages/div's own Div module was the template, but doesn't fit
# here) -- several of these genuinely recurse, and not just each one
# calling itself: `coerce_literal` and `coerce_input_object_literal`
# call each other (nested input objects contain nested literal values,
# which can themselves be nested input objects), and `coerce_runtime_
# value` has the equivalent shape. A `module_function` method calling
# *itself* via a qualified call (`ModuleName.method(...)`) was fixed at
# the compiler level 2026-08-24 (see ROADMAP.md's "Diamond-level
# findings worth remembering"), but mutual recursion between two
# *different* module_function siblings -- what this file actually
# needs -- is explicitly still out of scope for that fix (whichever
# sibling is defined first can't yet see the other's not-yet-registered
# descriptor). A class's own `self.` method has no such ordering
# requirement -- confirmed directly against a throwaway fixture -- so
# every call below uses `self.`, not `ClassName.method(...)`.
#
# List/nested-value handling below uses explicit index loops rather
# than `Array#map` -- not working around a bug (a plain nested `def`
# handed to `.map()` inside a `self.` method DOES fail Callable arity-
# checking, but that turned out to be correct behavior for the wrong
# syntax, not a compiler bug: `docs/syntax.md`'s "closure name() ...
# end" section documents a plain nested `def` as "built for exactly
# one job: a detached patch, meant to be handed to define_method/
# redefine_method" -- called directly instead, via `.map()` or
# otherwise, it's simply not the form for that; `closure name() ...
# end` is. Confirmed directly: swapping `def` for `closure` in the
# exact same `.map()` shape fixes the arity mismatch with no other
# change). Plain loops here are just this package's own established
# style (matching every other package in this repo), not a forced
# workaround.
module GraphQL
  module Execution
    class Coercion
      # The five built-in scalars are always valid in a variable's own type
      # reference (`$flag: Boolean!`), regardless of whether the schema's
      # own type graph happens to use them anywhere reachable from its
      # query/mutation roots -- Schema#type_map only walks *reachable*
      # types, so a schema with no Boolean-typed field/argument anywhere
      # would otherwise make `$flag: Boolean!` fail to resolve, a real bug
      # caught by testing (a fragment/directive smoke test using
      # `$skipName: Boolean!` against a schema with no Boolean field
      # anywhere). nil for any other name -- the caller falls through to
      # its own "unknown type" error for anything user-defined that
      # genuinely isn't reachable.
      def self.built_in_scalar(name)
        if name == "String"
          GraphQL::ScalarType.string()
        elsif name == "Int"
          GraphQL::ScalarType.int()
        elsif name == "Float"
          GraphQL::ScalarType.float()
        elsif name == "Boolean"
          GraphQL::ScalarType.boolean()
        elsif name == "ID"
          GraphQL::ScalarType.id()
        else
          nil
        end
      end

      # Resolves a language-layer type *reference* (NamedType/ListType/
      # NonNullType, as parsed out of a VariableDefinition's own `: Type`
      # clause) against the schema's real type registry, producing an
      # actual GraphQL::Type.
      def self.resolve_type_reference(type_ref, type_map)
        if type_ref is GraphQL::Language::NonNullType
          self.resolve_type_reference(type_ref.of_type(), type_map).non_null()
        elsif type_ref is GraphQL::Language::ListType
          self.resolve_type_reference(type_ref.of_type(), type_map).list()
        else
          resolved = type_map[type_ref.name()]
          if resolved == nil
            resolved = self.built_in_scalar(type_ref.name())
          end
          if resolved == nil
            raise GraphQL::RequestError.new("unknown type \"#{type_ref.name()}\"")
          end
          resolved
        end
      end

      # Coerces a literal AST value node -- or a plain Diamond scalar,
      # stored directly rather than wrapped, per nodes.di's own convention
      # -- against `type`, substituting `$variable` references from
      # `coerced_variables`. Shared by argument literal-value coercion and
      # variable-definition default-value coercion.
      def self.coerce_literal(value, type, coerced_variables)
        if value is GraphQL::Language::Variable
          coerced_variables[value.name()]
        elsif type.kind() == "NON_NULL"
          if value is GraphQL::Language::NullValue
            raise GraphQL::RequestError.new("null given for non-null type \"#{type.name()}\"")
          end
          self.coerce_literal(value, type.of_type(), coerced_variables)
        elsif value is GraphQL::Language::NullValue
          nil
        elsif type.kind() == "LIST"
          if value is GraphQL::Language::ListValue
            items = value.values()
            result = []
            index = 0
            while index < items.length()
              result.push(self.coerce_literal(items[index], type.of_type(), coerced_variables))
              index += 1
            end
            result
          else
            [self.coerce_literal(value, type.of_type(), coerced_variables)]
          end
        elsif type.kind() == "ENUM"
          unless value is GraphQL::Language::EnumValue
            raise GraphQL::RequestError.new("expected an enum value for \"#{type.name()}\"")
          end
          unless type.value_named?(value.name())
            raise GraphQL::RequestError.new("\"#{value.name()}\" is not a valid value for enum \"#{type.name()}\"")
          end
          value.name()
        elsif type.kind() == "INPUT_OBJECT"
          unless value is GraphQL::Language::ObjectValue
            raise GraphQL::RequestError.new("expected an input object for \"#{type.name()}\"")
          end
          self.coerce_input_object_literal(value, type, coerced_variables)
        else
          type.coerce_input()(value)
        end
      end

      def self.coerce_input_object_literal(object_value, type, coerced_variables)
        provided = {}
        index = 0
        while index < object_value.fields().length()
          field = object_value.fields()[index]
          provided[field.name()] = field.value()
          index += 1
        end
        result = {}
        index = 0
        while index < type.arguments().length()
          arg = type.arguments()[index]
          if provided.include_key?(arg.name())
            result[arg.name()] = self.coerce_literal(provided[arg.name()], arg.type(), coerced_variables)
          elsif arg.has_default?()
            result[arg.name()] = arg.default_value()
          elsif arg.type().kind() == "NON_NULL"
            raise GraphQL::RequestError.new("missing required input field \"#{arg.name()}\" for \"#{type.name()}\"")
          end
          index += 1
        end
        result
      end

      # Coerces a RAW runtime value -- an already-decoded Diamond Hash/
      # Array/Int/Float/String/Bool/nil, e.g. one entry of the caller's own
      # `variables` Hash passed into Schema#execute -- against `type`.
      # Mirrors coerce_literal's own shape but over plain Diamond values
      # instead of language-layer AST nodes, since a caller-supplied
      # variables Hash was never parsed out of GraphQL query-document text.
      def self.coerce_runtime_value(value, type)
        if type.kind() == "NON_NULL"
          if value == nil
            raise GraphQL::RequestError.new("null given for non-null type \"#{type.name()}\"")
          end
          self.coerce_runtime_value(value, type.of_type())
        elsif value == nil
          nil
        elsif type.kind() == "LIST"
          if value is Array
            result = []
            index = 0
            while index < value.length()
              result.push(self.coerce_runtime_value(value[index], type.of_type()))
              index += 1
            end
            result
          else
            [self.coerce_runtime_value(value, type.of_type())]
          end
        elsif type.kind() == "ENUM"
          unless value is String
            raise GraphQL::RequestError.new("expected a String enum value for \"#{type.name()}\"")
          end
          unless type.value_named?(value)
            raise GraphQL::RequestError.new("\"#{value}\" is not a valid value for enum \"#{type.name()}\"")
          end
          value
        elsif type.kind() == "INPUT_OBJECT"
          unless value is Hash
            raise GraphQL::RequestError.new("expected an object for \"#{type.name()}\"")
          end
          result = {}
          index = 0
          while index < type.arguments().length()
            arg = type.arguments()[index]
            if value.include_key?(arg.name())
              result[arg.name()] = self.coerce_runtime_value(value[arg.name()], arg.type())
            elsif arg.has_default?()
              result[arg.name()] = arg.default_value()
            elsif arg.type().kind() == "NON_NULL"
              raise GraphQL::RequestError.new("missing required input field \"#{arg.name()}\" for \"#{type.name()}\"")
            end
            index += 1
          end
          result
        else
          type.coerce_input()(value)
        end
      end

      # Every declared variable name -> its final coerced value (from the
      # caller's raw `variables`, else the declaration's own default, else
      # nil for an optional variable never given one) -- raises if a
      # non-null variable ends up with neither.
      def self.coerce_variable_definitions(variable_definitions, raw_variables, type_map)
        result = {}
        index = 0
        while index < variable_definitions.length()
          vardef = variable_definitions[index]
          schema_type = self.resolve_type_reference(vardef.type(), type_map)
          if raw_variables.include_key?(vardef.name())
            result[vardef.name()] = self.coerce_runtime_value(raw_variables[vardef.name()], schema_type)
          elsif vardef.default_value() != nil
            result[vardef.name()] = self.coerce_literal(vardef.default_value(), schema_type, {})
          elsif schema_type.kind() == "NON_NULL"
            raise GraphQL::RequestError.new("missing required variable \"$#{vardef.name()}\"")
          else
            result[vardef.name()] = nil
          end
          index += 1
        end
        result
      end

      # The final args Hash for one field/directive call: a schema-declared
      # argument not present in `ast_arguments` falls back to its own
      # default (or is simply omitted if optional with none, or raises if
      # required).
      def self.coerce_arguments(ast_arguments, schema_arguments, coerced_variables)
        provided = {}
        index = 0
        while index < ast_arguments.length()
          argument = ast_arguments[index]
          provided[argument.name()] = argument.value()
          index += 1
        end
        result = {}
        index = 0
        while index < schema_arguments.length()
          arg = schema_arguments[index]
          if provided.include_key?(arg.name())
            result[arg.name()] = self.coerce_literal(provided[arg.name()], arg.type(), coerced_variables)
          elsif arg.has_default?()
            result[arg.name()] = arg.default_value()
          elsif arg.type().kind() == "NON_NULL"
            raise GraphQL::RequestError.new("missing required argument \"#{arg.name()}\"")
          end
          index += 1
        end
        result
      end
    end

  end
end
