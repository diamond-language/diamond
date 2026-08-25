# The core deliverable: walks a parsed query Document against a built
# Schema, calling resolvers and assembling the `{"data": ..., "errors":
# [...]}` result. A synchronous tree-walking implementation of the
# official GraphQL spec's "Executing Requests" chapter (ExecuteRequest
# -> ExecuteSelectionSet -> ExecuteField -> CompleteValue) -- not a
# transliteration of graphql-ruby's own dataloader/Fiber-aware
# execution/interpreter/runtime.rb, which solves a different, harder
# problem (lazy/batched resolution) this v1 deliberately doesn't cover.
# Resolvers are plain synchronous Diamond calls returning values
# directly.
#
# A class with `self.`/instance methods, not a `module_function`
# module -- same reasoning as execution/coercion.di's own header
# comment (a `module_function` method can't call itself, even spelled
# `ModuleName.method(...)`; only a class's own `self.method(...)`-style
# recursive self-call works, confirmed against a throwaway fixture).
# Errors, `path`, and every other piece of per-request state live on
# `@errors`/instance state, so `Schema#execute` builds one fresh
# `Executor` per call (see schema.di) -- no state to reset between
# requests, no shared mutable state to worry about.
module GraphQL
module Execution

# Internal signal only -- never part of this package's public API, a
# resolver should never rescue this itself. Raised by #complete_value
# when a Non-Null-typed position completes to null, and caught exactly
# once per "boundary" (a field's own declared type in #execute_field,
# or a list's own item type in #complete_list_value): the boundary
# closest to the origin already pushed a real error onto `@errors`
# (either its own StandardError catch, or by not catching this class
# at all and letting the *first* StandardError rescue below record it)
# -- every ancestor boundary this then bubbles through just re-raises
# without recording again, until it reaches one whose own type is
# nullable, where it's absorbed into a plain `nil` result for that
# position. Mirrors graphql-js's own completeValue/handleFieldError
# split, using a real Diamond exception instead of a sentinel value
# threaded through every call.
class NullBubbleError < StandardError
  attr_reader message: String
  def initialize(message: String)
    @message = message
  end
end

class Executor
  def initialize(schema)
    @schema = schema
    @type_map = schema.type_map()
    @errors = []
  end

  # `raw_variables`/`context`/`root_value` all default to the "nothing
  # given" shape a caller with no need for them can ignore entirely.
  # `operation_name` is required only when `query_string` defines more
  # than one operation.
  def execute(query_string, raw_variables = {}, context = {}, root_value = nil, operation_name = nil)
    begin
      document = GraphQL::Language::Parser.parse(query_string)
    rescue e: GraphQL::Language::ParseError
      return {"errors": [{"message": e.message()}]}
    end

    validation_errors = GraphQL::Validation::Validator.validate(document, @schema)
    if validation_errors.length() > 0
      errors = []
      index = 0
      while index < validation_errors.length()
        errors.push({"message": validation_errors[index]})
        index += 1
      end
      return {"errors": errors}
    end

    begin
      operation = self.find_operation(document, operation_name)
    rescue e: GraphQL::RequestError
      return {"errors": [{"message": e.message()}]}
    end

    root_type = self.root_type_for(operation)
    if root_type == nil
      return {"errors": [{"message": "schema has no #{operation.operation()} type"}]}
    end

    fragments = self.index_fragments(document)

    begin
      coerced_variables = GraphQL::Execution::Coercion.coerce_variable_definitions(
        operation.variable_definitions(), raw_variables, @type_map)
    rescue e: GraphQL::RequestError
      return {"errors": [{"message": e.message()}]}
    end

    data = nil
    begin
      data = self.execute_selection_set(operation.selection_set(), root_type, root_value,
        context, coerced_variables, fragments, [])
    rescue e: NullBubbleError
      data = nil
    end

    if @errors.length() > 0
      {"data": data, "errors": @errors}
    else
      {"data": data}
    end
  end

  def root_type_for(operation)
    kind = operation.operation()
    if kind == "mutation"
      @schema.mutation_type()
    elsif kind == "subscription"
      nil
    else
      @schema.query_type()
    end
  end

  def index_fragments(document)
    result = {}
    index = 0
    while index < document.definitions().length()
      definition = document.definitions()[index]
      if definition is GraphQL::Language::FragmentDefinition
        result[definition.name()] = definition
      end
      index += 1
    end
    result
  end

  def find_operation(document, operation_name)
    operations = []
    index = 0
    while index < document.definitions().length()
      definition = document.definitions()[index]
      if definition is GraphQL::Language::OperationDefinition
        operations.push(definition)
      end
      index += 1
    end
    if operation_name == nil
      if operations.length() == 1
        operations[0]
      else
        raise GraphQL::RequestError.new(
          "must provide an operation name when the document defines more than one operation")
      end
    else
      found = nil
      index = 0
      while index < operations.length() && found == nil
        if operations[index].name() == operation_name
          found = operations[index]
        end
        index += 1
      end
      if found == nil
        raise GraphQL::RequestError.new("unknown operation \"#{operation_name}\"")
      end
      found
    end
  end

  # true/false/nil (nil meaning: this directive wasn't present at all).
  def directive_value(directives, directive_name, coerced_variables)
    result = nil
    index = 0
    while index < directives.length()
      directive = directives[index]
      if directive.name() == directive_name
        if_value = nil
        arg_index = 0
        while arg_index < directive.arguments().length()
          if directive.arguments()[arg_index].name() == "if"
            if_value = directive.arguments()[arg_index].value()
          end
          arg_index += 1
        end
        result = GraphQL::Execution::Coercion.coerce_literal(if_value, GraphQL::ScalarType.boolean(), coerced_variables)
      end
      index += 1
    end
    result
  end

  def selection_included?(directives, coerced_variables)
    if self.directive_value(directives, "skip", coerced_variables) == true
      return false
    end
    if self.directive_value(directives, "include", coerced_variables) == false
      return false
    end
    true
  end

  # Does `type_condition` (a fragment/inline-fragment's own `on Type`,
  # or nil for an untyped inline fragment) apply to a concrete
  # `object_type`? True for a nil condition (always applies), an exact
  # name match, an interface `object_type` implements, or a union
  # `object_type` is a member of.
  def type_condition_applies?(type_condition, object_type)
    if type_condition == nil
      return true
    end
    if type_condition == object_type.name()
      return true
    end
    named = @type_map[type_condition]
    if named == nil
      return false
    end
    if named.kind() == "INTERFACE"
      interfaces = object_type.interfaces()
      index = 0
      while index < interfaces.length()
        if interfaces[index].name() == type_condition
          return true
        end
        index += 1
      end
      false
    elsif named.kind() == "UNION"
      named.includes?(object_type.name())
    else
      false
    end
  end

  # Flattens `selection_set` into an ordered map (a Hash, insertion-
  # order-preserving) of response_key -> Array<Field AST node>, resolving
  # fragment spreads/inline fragments and @skip/@include along the way.
  # `grouped`/`visited_fragments` are accumulators mutated in place
  # (Hash/Array are reference types here) so recursive fragment
  # expansion folds directly into the caller's own result.
  def collect_fields(selection_set, object_type, coerced_variables, fragments, visited_fragments, grouped)
    index = 0
    while index < selection_set.length()
      selection = selection_set[index]
      if selection is GraphQL::Language::Field
        if self.selection_included?(selection.directives(), coerced_variables)
          key = selection.response_key()
          if grouped.keys().include?(key)
            grouped[key].push(selection)
          else
            grouped[key] = [selection]
          end
        end
      elsif selection is GraphQL::Language::FragmentSpread
        if self.selection_included?(selection.directives(), coerced_variables) &&
           !visited_fragments.include?(selection.name())
          visited_fragments.push(selection.name())
          fragment = fragments[selection.name()]
          if fragment != nil && self.type_condition_applies?(fragment.type_condition(), object_type)
            self.collect_fields(fragment.selection_set(), object_type, coerced_variables,
              fragments, visited_fragments, grouped)
          end
        end
      elsif selection is GraphQL::Language::InlineFragment
        if self.selection_included?(selection.directives(), coerced_variables) &&
           self.type_condition_applies?(selection.type_condition(), object_type)
          self.collect_fields(selection.selection_set(), object_type, coerced_variables,
            fragments, visited_fragments, grouped)
        end
      end
      index += 1
    end
    grouped
  end

  def execute_selection_set(selection_set, object_type, object_value, context, coerced_variables, fragments, path)
    grouped = self.collect_fields(selection_set, object_type, coerced_variables, fragments, [], {})
    result = {}
    keys = grouped.keys()
    index = 0
    while index < keys.length()
      key = keys[index]
      fields = grouped[key]
      field_path = path.concat([key])
      result[key] = self.execute_field(object_type, object_value, fields, context, coerced_variables, fragments, field_path)
      index += 1
    end
    result
  end

  # `fields` is every Field AST node matched at this response key (more
  # than one when fragments merge into the same key) -- name/arguments
  # come from the first occurrence (this v1 doesn't validate that
  # merged occurrences declare identical arguments, matching this
  # package's own deliberately-partial validation scope); every
  # occurrence's own selection_set is merged together in
  # #merged_selection_set for whatever completes this field's value.
  def execute_field(object_type, object_value, fields, context, coerced_variables, fragments, path)
    field_node = fields[0]
    field_name = field_node.name()
    if field_name == "__typename"
      return object_type.name()
    end
    schema_field = object_type.field_named(field_name)
    if schema_field == nil
      @errors.push({"message": "field \"#{field_name}\" not found on type \"#{object_type.name()}\"", "path": path})
      return nil
    end
    begin
      args = GraphQL::Execution::Coercion.coerce_arguments(field_node.arguments(), schema_field.arguments(), coerced_variables)
      resolve = schema_field.resolve()
      raw_result = resolve(object_value, args, context)
      self.complete_value(schema_field.type(), fields, raw_result, context, coerced_variables, fragments, path)
    rescue e: NullBubbleError
      if schema_field.type().kind() == "NON_NULL"
        raise e
      end
      nil
    rescue e: StandardError
      @errors.push({"message": e.message(), "path": path})
      if schema_field.type().kind() == "NON_NULL"
        raise NullBubbleError.new(e.message())
      end
      nil
    end
  end

  def merged_selection_set(fields)
    result = []
    index = 0
    while index < fields.length()
      selection_set = fields[index].selection_set()
      unless selection_set == nil
        result = result.concat(selection_set)
      end
      index += 1
    end
    result
  end

  def resolve_abstract_type(type, result, context)
    resolver = type.type_resolver()
    if resolver == nil
      raise GraphQL::ExecutionError.new("no resolve_type configured for \"#{type.name()}\"")
    end
    concrete_name = resolver(result, context)
    concrete_type = @type_map[concrete_name]
    if concrete_type == nil
      raise GraphQL::ExecutionError.new(
        "resolve_type for \"#{type.name()}\" returned unknown type \"#{concrete_name}\"")
    end
    concrete_type
  end

  def complete_value(type, fields, result, context, coerced_variables, fragments, path)
    if type.kind() == "NON_NULL"
      completed = self.complete_value(type.of_type(), fields, result, context, coerced_variables, fragments, path)
      if completed == nil
        # A genuine origin, not a propagation from further down: the
        # recursive call above only ever returns nil (rather than
        # raising) when `result` itself was nil at this exact unwrap
        # level -- any deeper Non-Null violation already propagated as
        # a NullBubbleError exception instead of returning normally, so
        # this is always the first boundary to see this particular
        # failure. Record it here, once.
        message = "cannot return null for a non-null field"
        @errors.push({"message": message, "path": path})
        raise NullBubbleError.new(message)
      end
      completed
    elsif result == nil
      nil
    elsif type.kind() == "LIST"
      self.complete_list_value(type, fields, result, context, coerced_variables, fragments, path)
    elsif type.kind() == "OBJECT"
      self.execute_selection_set(self.merged_selection_set(fields), type, result, context, coerced_variables, fragments, path)
    elsif type.kind() == "INTERFACE" || type.kind() == "UNION"
      concrete_type = self.resolve_abstract_type(type, result, context)
      self.execute_selection_set(self.merged_selection_set(fields), concrete_type, result, context, coerced_variables, fragments, path)
    elsif type.kind() == "ENUM"
      # EnumType has no coerce_result Callable (unlike ScalarType) --
      # its "coercion" is just membership-checking a resolver's own
      # returned String against the type's declared value names.
      unless result is String && type.value_named?(result)
        raise GraphQL::ExecutionError.new("\"#{result}\" is not a valid value for enum \"#{type.name()}\"")
      end
      result
    else
      coercer = type.coerce_result()
      coercer(result)
    end
  end

  def complete_list_value(type, fields, result, context, coerced_variables, fragments, path)
    unless result is Array
      raise GraphQL::ExecutionError.new("expected a list for \"#{type.name()}\"")
    end
    item_type = type.of_type()
    output = []
    index = 0
    while index < result.length()
      item_path = path.concat([index])
      begin
        output.push(self.complete_value(item_type, fields, result[index], context, coerced_variables, fragments, item_path))
      rescue e: NullBubbleError
        if item_type.kind() == "NON_NULL"
          raise e
        end
        output.push(nil)
      rescue e: StandardError
        @errors.push({"message": e.message(), "path": item_path})
        if item_type.kind() == "NON_NULL"
          raise NullBubbleError.new(e.message())
        end
        output.push(nil)
      end
      index += 1
    end
    output
  end
end

end
end
