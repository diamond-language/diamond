# AST node classes produced by Parser (parser.di) from GraphQL
# query-document text (operations/fragments/selections, not schema SDL
# -- this package's schema is built with Diamond code, see
# GraphQL::Type and friends, not parsed from `.graphql` files, so there
# are no ObjectTypeDefinition/ScalarTypeDefinition/SchemaDefinition-style
# nodes here the way graphql-ruby's own nodes.rb has).
#
# Plain data classes -- attr_reader-only, no behavior. A literal scalar
# value (Int/Float/String/Bool) used as an argument/default value is
# stored directly as that native Diamond value, no wrapper node; only
# the handful of value *kinds* execution needs to tell apart from an
# ordinary literal get their own node class below (Variable, NullValue,
# EnumValue, ListValue, ObjectValue).
#
# Deliberately namespaced under GraphQL::Language, distinct from the
# schema-side GraphQL::Field/GraphQL::Argument (packages/graphql's type
# system) despite the overlapping short names (Field, Argument) -- see
# this package's ROADMAP.md for the known compiler gotcha around
# same-named classes in different namespaces, confirmed clear of it
# here since both this file and the schema side use the fully
# `GraphQL::Language::`/`GraphQL::`-qualified name at every reference,
# never a bare unqualified one.
module GraphQL
module Language

class Document
  attr_reader definitions: Array
  def initialize(definitions: Array)
    @definitions = definitions
  end
end

# operation: "query" | "mutation" | "subscription". name is nil for an
# anonymous (shorthand `{ ... }`) query.
class OperationDefinition
  attr_reader operation: String
  attr_reader name
  attr_reader variable_definitions: Array
  attr_reader directives: Array
  attr_reader selection_set: Array
  def initialize(operation: String, name, variable_definitions: Array,
                 directives: Array, selection_set: Array)
    @operation = operation
    @name = name
    @variable_definitions = variable_definitions
    @directives = directives
    @selection_set = selection_set
  end
end

# type is a NamedType/ListType/NonNullType (this file's own, syntax-only
# type references, not a schema GraphQL::Type). default_value is nil
# when the source gave none -- distinct from an explicit `null` default,
# which parses to a NullValue instance instead.
class VariableDefinition
  attr_reader name: String
  attr_reader type
  attr_reader default_value
  def initialize(name: String, type, default_value)
    @name = name
    @type = type
    @default_value = default_value
  end
end

# alias_name is nil when the field wasn't aliased. selection_set is nil
# for a leaf field (`id`) -- distinct from an empty Array, which isn't
# reachable here since GraphQL's own grammar has no `{}` empty
# selection set, only "no selection set at all" or "one with >=1
# selections in it."
class Field
  attr_reader alias_name
  attr_reader name: String
  attr_reader arguments: Array
  attr_reader directives: Array
  attr_reader selection_set: Array | Nil
  def initialize(alias_name, name: String, arguments: Array,
                 directives: Array, selection_set: Array | Nil)
    @alias_name = alias_name
    @name = name
    @arguments = arguments
    @directives = directives
    @selection_set = selection_set
  end

  # The name a response key uses: the alias if one was given, else the
  # field name itself -- every caller that needs "the key this field's
  # result lands under" wants this, not @name/@alias_name separately.
  def response_key()
    if @alias_name == nil
      @name
    else
      @alias_name
    end
  end
end

class Argument
  attr_reader name: String
  attr_reader value
  def initialize(name: String, value)
    @name = name
    @value = value
  end
end

class Directive
  attr_reader name: String
  attr_reader arguments: Array
  def initialize(name: String, arguments: Array)
    @name = name
    @arguments = arguments
  end
end

class FragmentDefinition
  attr_reader name: String
  attr_reader type_condition: String
  attr_reader directives: Array
  attr_reader selection_set: Array
  def initialize(name: String, type_condition: String, directives: Array,
                 selection_set: Array)
    @name = name
    @type_condition = type_condition
    @directives = directives
    @selection_set = selection_set
  end
end

class FragmentSpread
  attr_reader name: String
  attr_reader directives: Array
  def initialize(name: String, directives: Array)
    @name = name
    @directives = directives
  end
end

# type_condition is nil for an untyped `... { ... }` inline fragment.
class InlineFragment
  attr_reader type_condition
  attr_reader directives: Array
  attr_reader selection_set: Array
  def initialize(type_condition, directives: Array, selection_set: Array)
    @type_condition = type_condition
    @directives = directives
    @selection_set = selection_set
  end
end

# A `$name` reference used as a value (an argument value, a default
# value, ...) -- distinct from a literal so the executor can tell "look
# this up in the coerced variables" apart from "use this value as-is."
class Variable
  attr_reader name: String
  def initialize(name: String)
    @name = name
  end
end

# Marker for a literal `null` value -- distinct from Diamond's own nil
# (which this package instead uses for "this optional slot was never
# given at all," e.g. Field#alias_name / VariableDefinition#default_value
# when the source simply didn't include one).
class NullValue
  def initialize()
  end
end

# A bare identifier used as a value, e.g. `RED` in `color: RED` --
# distinct from Variable (no `$`) and from a NAME used as a field/arg
# name elsewhere in the grammar.
class EnumValue
  attr_reader name: String
  def initialize(name: String)
    @name = name
  end
end

class ListValue
  attr_reader values: Array
  def initialize(values: Array)
    @values = values
  end
end

class ObjectField
  attr_reader name: String
  attr_reader value
  def initialize(name: String, value)
    @name = name
    @value = value
  end
end

class ObjectValue
  attr_reader fields: Array
  def initialize(fields: Array)
    @fields = fields
  end
end

# Type references used in a VariableDefinition's own `type:` -- distinct
# from the schema's runtime GraphQL::Type hierarchy (GraphQL::ObjectType
# et al.): these three are pure syntax, just naming a type by String,
# resolved against the schema's real types later by whichever code
# coerces a variable's declared type (execution/coercion.di).
class NamedType
  attr_reader name: String
  def initialize(name: String)
    @name = name
  end
end

class ListType
  attr_reader of_type
  def initialize(of_type)
    @of_type = of_type
  end
end

class NonNullType
  attr_reader of_type
  def initialize(of_type)
    @of_type = of_type
  end
end

end
end
