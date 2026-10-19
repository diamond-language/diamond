# A deliberate SUBSET of the spec's ~30 validation rules -- ported:
# field existence, known/required arguments (plus unique argument
# names), leaf-vs-composite selection-set correctness, fragment type
# conditions reference a real composite type, unique operation/fragment
# names, at most one anonymous operation, undefined-variable usage, and
# a simplified (not 100%-spec) variable/argument type-compatibility
# check. Deliberately NOT ported (real spec rules, real complexity, no
# known consumer need yet -- see ROADMAP.md): overlapping-fields-can-
# be-merged, fragment cycle detection, directive-valid-location
# checking, and the spec's own "a nullable variable is still allowed
# where non-null is expected if the location has a non-null default"
# exception to type-compatibility (this file's own #type_compatible?
# always requires the variable itself to be declared non-null). Also
# not validated: anything *inside* a `__schema`/`__type` selection --
# #validate_field recognizes the two root introspection meta-fields
# themselves (name/args/needs-a-sub-selection), but doesn't recurse
# into their own sub-selections the way it does for an ordinary
# composite field, since doing so would need this file to build its
# own copy of introspection.di's meta-object-types just to validate
# against (a lot of parameter-threading for a low-stakes edge case --
# a typo'd nested introspection field still gets rejected correctly,
# just as an execution-time field error via the executor's own
# defensive fallback rather than a validation-time one).
#
# `Validator.validate(document, schema)` returns an Array of message
# Strings (empty when the document is valid) -- collects every problem
# it finds rather than failing fast on the first one, since a client
# fixing one query error at a time off of a single reported message is
# a worse experience than seeing everything wrong at once (this is also
# how graphql-ruby's own validator behaves).
#
# A class with `self.` methods, not `module_function` -- same reasoning
# as execution/coercion.di and execution/executor.di's own header
# comments: `#validate_selection_set` and `#validate_field` call each
# other (a composite field's own selection set validates its child
# fields, one of which can itself have a nested selection set), and a
# `module_function` sibling calling a sibling defined later in the same
# module still doesn't work (only straight self-recursion via
# `ModuleName.method(...)` was fixed, 2026-08-24 -- see ROADMAP.md). A
# class's own `self.method(...)` self/mutual call has no such ordering
# requirement.
module GraphQL
  module Validation

    class Validator
      def self.validate(document, schema)
        errors = []
        type_map = schema.type_map()
        fragments = self.index_fragments(document)
        operations = self.find_operations(document)

        self.check_lone_anonymous_operation(operations, errors)
        self.check_unique_operation_names(operations, errors)
        self.check_unique_fragment_names(document, errors)
        self.check_fragment_type_conditions(document, type_map, errors)
        self.check_fragment_cycles(document, errors)

        # Fragments let a small document expand into an enormous one, so the size is measured (fragments included) before anything walks it.
        self.check_query_limits(operations, fragments, schema, errors)
        if errors.length() > 0 && self.limit_error?(errors)
          return errors
        end

        index = 0
        while index < operations.length()
          self.validate_operation(operations[index], schema, type_map, fragments, errors)
          index += 1
        end
        errors
      end

      def self.index_fragments(document)
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

      def self.find_operations(document)
        result = []
        index = 0
        while index < document.definitions().length()
          definition = document.definitions()[index]
          if definition is GraphQL::Language::OperationDefinition
            result.push(definition)
          end
          index += 1
        end
        result
      end

      def self.check_lone_anonymous_operation(operations, errors)
        anonymous_count = 0
        index = 0
        while index < operations.length()
          if operations[index].name() == nil
            anonymous_count += 1
          end
          index += 1
        end
        if anonymous_count > 0 && operations.length() > 1
          errors.push("this document must have only one operation when it includes an anonymous operation")
        end
      end

      def self.check_unique_operation_names(operations, errors)
        seen = []
        index = 0
        while index < operations.length()
          name = operations[index].name()
          if name != nil
            if seen.include?(name)
              errors.push("duplicate operation name \"#{name}\"")
            else
              seen.push(name)
            end
          end
          index += 1
        end
      end

      def self.check_unique_fragment_names(document, errors)
        seen = []
        index = 0
        while index < document.definitions().length()
          definition = document.definitions()[index]
          if definition is GraphQL::Language::FragmentDefinition
            if seen.include?(definition.name())
              errors.push("duplicate fragment name \"#{definition.name()}\"")
            else
              seen.push(definition.name())
            end
          end
          index += 1
        end
      end

      # The meta types a document may name in a type condition (`fragment F on __Type`, as introspection tools write
      # it). They are not in the schema's type map, and nothing inside an introspection selection is validated.
      def self.introspection_type?(name) = ["__Schema", "__Type", "__Field", "__InputValue", "__EnumValue", "__Directive"].include?(name)

      # Fragment spreads inside `selections`, nested selection sets and inline fragments included, as names.
      def self.spreads_in(selections, found)
        selections.each() do |selection|
          if selection is GraphQL::Language::FragmentSpread
            found.push(selection.name())
          elsif selection is GraphQL::Language::InlineFragment
            self.spreads_in(selection.selection_set(), found)
          elsif selection is GraphQL::Language::Field
            if selection.selection_set() != nil then self.spreads_in(selection.selection_set(), found) end
          end
        end
        found
      end

      # A fragment may not spread itself, directly or through others. Kahn's algorithm over the spread graph (no
      # recursion, so a long chain of fragments cannot exhaust the stack): fragments that spread only resolved
      # fragments are resolved in turn, and whatever is left over sits on a cycle or leads into one.
      def self.check_fragment_cycles(document, errors)
        spreads = {}
        document.definitions().each() do |definition|
          if definition is GraphQL::Language::FragmentDefinition
            spreads[definition.name()] = self.spreads_in(definition.selection_set(), []).uniq().select() do |name| name != nil end
          end
        end
        waiting = {}
        dependents = {}
        spreads.keys().each() do |name|
          known = spreads[name].select() do |other| spreads.has_key?(other) end
          waiting[name] = known.length()
          known.each() do |other|
            if !dependents.has_key?(other) then dependents[other] = [] end
            dependents[other].push(name)
          end
        end
        ready = spreads.keys().select() do |name| waiting[name] == 0 end
        resolved = 0
        while !ready.empty?()
          name = ready.pop()
          resolved += 1
          if dependents.has_key?(name)
            dependents[name].each() do |waiter|
              waiting[waiter] -= 1
              if waiting[waiter] == 0 then ready.push(waiter) end
            end
          end
        end
        if resolved < spreads.length()
          stuck = spreads.keys().select() do |name| waiting[name] > 0 end
          # Fragments that only lead into a cycle are not on it: peel those nothing stuck spreads, repeatedly.
          users = {}
          stuck.each() do |name| users[name] = 0 end
          stuck.each() do |name|
            spreads[name].each() do |other|
              if users.has_key?(other) then users[other] += 1 end
            end
          end
          peel = stuck.select() do |name| users[name] == 0 end
          peeled = {}
          while !peel.empty?()
            name = peel.pop()
            peeled[name] = true
            spreads[name].each() do |other|
              if users.has_key?(other) && !peeled.has_key?(other)
                users[other] -= 1
                if users[other] == 0 then peel.push(other) end
              end
            end
          end
          stuck = stuck.select() do |name| !peeled.has_key?(name) end
          errors.push("fragments cannot spread themselves, directly or through others (cycle through #{stuck.map() do |name| "\"#{name}\"" end.join(", ")})")
        end
      end

      def self.check_fragment_type_conditions(document, type_map, errors)
        index = 0
        while index < document.definitions().length()
          definition = document.definitions()[index]
          if definition is GraphQL::Language::FragmentDefinition
            named = type_map[definition.type_condition()]
            if named == nil && self.introspection_type?(definition.type_condition())
              named = nil
            elsif named == nil
              errors.push("fragment \"#{definition.name()}\" targets unknown type \"#{definition.type_condition()}\"")
            elsif !(named.kind() == "OBJECT" || named.kind() == "INTERFACE" || named.kind() == "UNION")
              errors.push("fragment \"#{definition.name()}\" cannot target non-composite type \"#{definition.type_condition()}\"")
            end
          end
          index += 1
        end
      end

      def self.limit_error?(errors)
        found = false
        errors.each() do |message|
          found = true if message.start_with?("query is too large:")
        end
        found
      end

      # [deepest field nesting, number of fields] of a selection set, with fragment spreads expanded. Memoized per fragment, so a document that
      # nests fragments 40 deep is measured in 40 steps although it would expand to 2**40 fields. A fragment already being measured (a cycle,
      # reported separately) counts as empty.
      def self.measure_selection_set(selection_set, fragments, memo, active)
        depth = 0
        nodes = 0
        index = 0
        while index < selection_set.length()
          selection = selection_set[index]
          part_depth = 0
          part_nodes = 0
          if selection is GraphQL::Language::Field
            if selection.selection_set() == nil
              part_depth = 1
              part_nodes = 1
            else
              inner = self.measure_selection_set(selection.selection_set(), fragments, memo, active)
              part_depth = 1 + inner[0]
              part_nodes = 1 + inner[1]
            end
          elsif selection is GraphQL::Language::FragmentSpread
            inner = self.measure_fragment(selection.name(), fragments, memo, active)
            part_depth = inner[0]
            part_nodes = inner[1]
          elsif selection is GraphQL::Language::InlineFragment
            inner = self.measure_selection_set(selection.selection_set(), fragments, memo, active)
            part_depth = inner[0]
            part_nodes = inner[1]
          end
          depth = part_depth if part_depth > depth
          nodes += part_nodes
          nodes = 1000000000000 if nodes > 1000000000000
          index += 1
        end
        [depth, nodes]
      end

      def self.measure_fragment(name, fragments, memo, active)
        return memo[name] if memo.has_key?(name)
        fragment = fragments[name]
        return [0, 0] if fragment == nil || active.include?(name)
        active.push(name)
        measured = self.measure_selection_set(fragment.selection_set(), fragments, memo, active)
        active.pop()
        memo[name] = measured
        measured
      end

      def self.check_query_limits(operations, fragments, schema, errors)
        memo = {}
        too_deep = false
        too_many = false
        index = 0
        while index < operations.length()
          measured = self.measure_selection_set(operations[index].selection_set(), fragments, memo, [])
          too_deep = true if measured[0] > schema.max_query_depth()
          too_many = true if measured[1] > schema.max_query_nodes()
          index += 1
        end
        if too_deep
          errors.push("query is too large: fields are nested more than #{schema.max_query_depth()} levels deep, fragments included")
        end
        if too_many
          errors.push("query is too large: it selects more than #{schema.max_query_nodes()} fields, fragments included")
        end
      end

      def self.validate_operation(operation, schema, type_map, fragments, errors)
        kind = operation.operation()
        root_type = if kind == "mutation" then schema.mutation_type() elsif kind == "subscription" then nil else schema.query_type() end
        if root_type == nil
          errors.push("schema has no #{kind} type")
        else
          declared_variables = self.index_variable_definitions(operation, type_map, errors)
          self.validate_selection_set(operation.selection_set(), root_type, schema.query_type(), type_map, fragments,
            declared_variables, errors, [])
        end
      end

      # var name -> its resolved GraphQL::Type (an unresolvable type
      # reference records its own error here and is simply omitted, so a
      # later "undefined variable" check against this Hash still fires for
      # any use of it, rather than compounding the same problem twice).
      def self.index_variable_definitions(operation, type_map, errors)
        result = {}
        definitions = operation.variable_definitions()
        index = 0
        while index < definitions.length()
          vardef = definitions[index]
          begin
            result[vardef.name()] = GraphQL::Execution::Coercion.resolve_type_reference(vardef.type(), type_map)
          rescue e: GraphQL::RequestError
            errors.push(e.message())
          end
          index += 1
        end
        result
      end

      def self.unwrap(type)
        if type.kind() == "LIST" || type.kind() == "NON_NULL"
          self.unwrap(type.of_type())
        else
          type
        end
      end

      def self.validate_selection_set(selection_set, parent_type, query_type, type_map, fragments, declared_variables, errors, visited_fragments)
        index = 0
        while index < selection_set.length()
          selection = selection_set[index]
          if selection is GraphQL::Language::Field
            self.validate_field(selection, parent_type, query_type, type_map, fragments, declared_variables, errors, visited_fragments)
          elsif selection is GraphQL::Language::FragmentSpread
            self.validate_directives(selection.directives(), declared_variables, errors)
            unless visited_fragments.include?(selection.name())
              visited_fragments.push(selection.name())
              fragment = fragments[selection.name()]
              if fragment == nil
                errors.push("unknown fragment \"#{selection.name()}\"")
              else
                fragment_type = type_map[fragment.type_condition()]
                unless fragment_type == nil
                  self.validate_selection_set(fragment.selection_set(), fragment_type, query_type, type_map, fragments,
                    declared_variables, errors, visited_fragments)
                end
              end
            end
          elsif selection is GraphQL::Language::InlineFragment
            self.validate_directives(selection.directives(), declared_variables, errors)
            target_type = parent_type
            if selection.type_condition() != nil
              named = type_map[selection.type_condition()]
              if named == nil && self.introspection_type?(selection.type_condition())
                target_type = parent_type
              elsif named == nil
                errors.push("inline fragment targets unknown type \"#{selection.type_condition()}\"")
              else
                target_type = named
              end
            end
            self.validate_selection_set(selection.selection_set(), target_type, query_type, type_map, fragments,
              declared_variables, errors, visited_fragments)
          end
          index += 1
        end
      end

      def self.validate_field(field_node, parent_type, query_type, type_map, fragments, declared_variables, errors, visited_fragments)
        if field_node.name() == "__typename"
          return nil
        end
        # __schema/__type are meta-fields of the query root specifically
        # (per spec), mirroring execution/executor.di's own identical
        # special-case (a reference-equality check against the schema's own
        # query_type, true only for a true top-level query field).
        if parent_type == query_type && field_node.name() == "__schema"
          self.validate_directives(field_node.directives(), declared_variables, errors)
          unless field_node.selection_set() != nil
            errors.push("field \"__schema\" of composite type \"__Schema\" must have a sub-selection")
          end
          return nil
        end
        if parent_type == query_type && field_node.name() == "__type"
          self.validate_directives(field_node.directives(), declared_variables, errors)
          self.validate_arguments(field_node.arguments(),
            [GraphQL::Argument.new("name", GraphQL::ScalarType.string().non_null())], declared_variables, errors, "__type")
          unless field_node.selection_set() != nil
            errors.push("field \"__type\" of composite type \"__Type\" must have a sub-selection")
          end
          return nil
        end
        unless parent_type.kind() == "OBJECT" || parent_type.kind() == "INTERFACE"
          errors.push("cannot select field \"#{field_node.name()}\" on non-composite type \"#{parent_type.name()}\"")
          return nil
        end
        schema_field = parent_type.field_named(field_node.name())
        if schema_field == nil
          errors.push("field \"#{field_node.name()}\" does not exist on type \"#{parent_type.name()}\"")
          return nil
        end
        self.validate_directives(field_node.directives(), declared_variables, errors)
        self.validate_arguments(field_node.arguments(), schema_field.arguments(), declared_variables, errors, field_node.name())

        named_field_type = self.unwrap(schema_field.type())
        is_leaf = named_field_type.kind() == "SCALAR" || named_field_type.kind() == "ENUM"
        has_selection = field_node.selection_set() != nil
        if is_leaf && has_selection
          errors.push("field \"#{field_node.name()}\" is a leaf type and cannot have a sub-selection")
        elsif !is_leaf && !has_selection
          errors.push("field \"#{field_node.name()}\" of composite type \"#{named_field_type.name()}\" must have a sub-selection")
        elsif has_selection
          self.validate_selection_set(field_node.selection_set(), named_field_type, query_type, type_map, fragments,
            declared_variables, errors, visited_fragments)
        end
      end

      def self.find_argument(schema_arguments, name)
        index = 0
        found = nil
        while index < schema_arguments.length() && found == nil
          if schema_arguments[index].name() == name
            found = schema_arguments[index]
          end
          index += 1
        end
        found
      end

      def self.find_argument_value(ast_arguments, name)
        index = 0
        found = nil
        while index < ast_arguments.length() && found == nil
          if ast_arguments[index].name() == name
            found = ast_arguments[index].value()
          end
          index += 1
        end
        found
      end

      def self.validate_arguments(ast_arguments, schema_arguments, declared_variables, errors, field_name)
        seen = []
        index = 0
        while index < ast_arguments.length()
          name = ast_arguments[index].name()
          if seen.include?(name)
            errors.push("duplicate argument \"#{name}\" on \"#{field_name}\"")
          else
            seen.push(name)
          end
          index += 1
        end

        index = 0
        while index < ast_arguments.length()
          arg_node = ast_arguments[index]
          schema_arg = self.find_argument(schema_arguments, arg_node.name())
          if schema_arg == nil
            errors.push("unknown argument \"#{arg_node.name()}\" on \"#{field_name}\"")
          else
            self.validate_value_variables(arg_node.value(), schema_arg.type(), declared_variables, errors)
          end
          index += 1
        end

        index = 0
        while index < schema_arguments.length()
          arg = schema_arguments[index]
          if arg.type().kind() == "NON_NULL" && !arg.has_default?()
            if self.find_argument_value(ast_arguments, arg.name()) == nil
              errors.push("missing required argument \"#{arg.name()}\" on \"#{field_name}\"")
            end
          end
          index += 1
        end
      end

      def self.validate_directives(directives, declared_variables, errors)
        index = 0
        while index < directives.length()
          directive = directives[index]
          arg_index = 0
          while arg_index < directive.arguments().length()
            self.validate_value_variables(directive.arguments()[arg_index].value(),
              GraphQL::ScalarType.boolean(), declared_variables, errors)
            arg_index += 1
          end
          index += 1
        end
      end

      # Checks every `$variable` reference reachable inside `value`
      # (recursing into list/object literals) against `declared_variables`
      # -- both that it was declared at all, and (a simplified, not-100%-
      # spec check, see this file's own header) that its declared type is
      # compatible with `expected_type`.
      def self.validate_value_variables(value, expected_type, declared_variables, errors)
        if value is GraphQL::Language::Variable
          unless declared_variables.include_key?(value.name())
            errors.push("undefined variable \"$#{value.name()}\"")
            return nil
          end
          variable_type = declared_variables[value.name()]
          unless self.type_compatible?(variable_type, expected_type)
            errors.push("variable \"$#{value.name()}\" of type \"#{variable_type.name()}\" is not compatible with expected type \"#{expected_type.name()}\"")
          end
        elsif value is GraphQL::Language::ListValue
          inner = self.list_item_type(expected_type)
          items = value.values()
          index = 0
          while index < items.length()
            self.validate_value_variables(items[index], inner, declared_variables, errors)
            index += 1
          end
        elsif value is GraphQL::Language::ObjectValue
          named = self.unwrap(expected_type)
          if named.kind() == "INPUT_OBJECT"
            fields = value.fields()
            index = 0
            while index < fields.length()
              field = fields[index]
              arg = self.find_argument(named.arguments(), field.name())
              unless arg == nil
                self.validate_value_variables(field.value(), arg.type(), declared_variables, errors)
              end
              index += 1
            end
          end
        end
      end

      # The element type a list-literal's own items are checked against --
      # unwraps one NON_NULL layer first if present, since `[T!]`/`[T]`
      # both have the same LIST-vs-not shape underneath.
      def self.list_item_type(expected_type)
        inner = if expected_type.kind() == "NON_NULL" then expected_type.of_type() else expected_type end
        if inner.kind() == "LIST"
          inner.of_type()
        else
          expected_type
        end
      end

      def self.type_compatible?(variable_type, expected_type)
        if expected_type.kind() == "NON_NULL"
          if variable_type.kind() != "NON_NULL"
            return false
          end
          return self.type_compatible?(variable_type.of_type(), expected_type.of_type())
        end
        if variable_type.kind() == "NON_NULL"
          return self.type_compatible?(variable_type.of_type(), expected_type)
        end
        if expected_type.kind() == "LIST"
          if variable_type.kind() != "LIST"
            return false
          end
          return self.type_compatible?(variable_type.of_type(), expected_type.of_type())
        end
        variable_type.name() == expected_type.name()
      end
    end

  end
end
