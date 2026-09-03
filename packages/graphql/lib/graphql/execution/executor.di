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
# comment: `#execute_field`, `#complete_value`, `#complete_list_value`,
# and `#execute_selection_set` below all call each other, not just
# themselves, and a `module_function` method calling a *different*
# module_function sibling only works if that sibling is already defined
# (fixed 2026-08-24 for straight self-recursion via
# `ModuleName.method(...)`, still not for this mutual shape -- see
# ROADMAP.md's "Diamond-level findings worth remembering"). A class's
# own `self.method(...)` self/mutual call has no such ordering
# requirement, confirmed against a throwaway fixture.
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
        meta_types = GraphQL::Introspection.build_meta_types()
        @schema_meta_type = meta_types["__Schema"]
        @type_meta_type = meta_types["__Type"]
        @type_name_argument = [GraphQL::Argument.new("name", GraphQL::ScalarType.string().non_null())]
        @dataloader = GraphQL::Execution::Dataloader.new()
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
        context["dataloader"] = @dataloader

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
          case selection
          when GraphQL::Language::Field
            if GraphQL::Execution::Directives.included?(selection.directives(), coerced_variables)
              key = selection.response_key()
              if grouped.include_key?(key)
                grouped[key].push(selection)
              else
                grouped[key] = [selection]
              end
            end
          when GraphQL::Language::FragmentSpread
            if GraphQL::Execution::Directives.included?(selection.directives(), coerced_variables) &&
               !visited_fragments.include?(selection.name())
              visited_fragments.push(selection.name())
              fragment = fragments[selection.name()]
              if fragment != nil && self.type_condition_applies?(fragment.type_condition(), object_type)
                self.collect_fields(fragment.selection_set(), object_type, coerced_variables,
                  fragments, visited_fragments, grouped)
              end
            end
          when GraphQL::Language::InlineFragment
            if GraphQL::Execution::Directives.included?(selection.directives(), coerced_variables) &&
               self.type_condition_applies?(selection.type_condition(), object_type)
              self.collect_fields(selection.selection_set(), object_type, coerced_variables,
                fragments, visited_fragments, grouped)
            end
          end
          index += 1
        end
        grouped
      end

      # Deliberately NOT fiber-wrapped, unlike #complete_list_value below --
      # confirmed directly (not assumed) that fiber-wrapping *this* loop
      # too actively BREAKS cross-item batching rather than extending it.
      # A field's own nested `def body` here would be a genuinely new Fiber
      # (`booksFiber`, say); when its own resolver hits `loader.load(...)`
      # and yields, that yield suspends *that* fresh fiber, not the
      # enclosing list-item's own fiber #complete_list_value is tracking --
      # so the `Dataloader#run` call spawned right here would see its own
      # local group "stuck" and dispatch immediately, with only *this one
      # item's* key ever having registered, before the outer
      # #complete_list_value loop ever gets a chance to resume the next
      # sibling item and let its own load() call join the same batch.
      # Caught by writing exactly that test (3 authors, each resolving
      # `books` via the same loader) and watching the batch function fire 3
      # times instead of 1 -- with this loop plain and sequential instead,
      # `yield` inside `loader.load()` correctly propagates up through
      # however many *ordinary* (non-fiber) nested calls sit above it
      # (`#execute_field`, this loop, `#complete_value`) to suspend the
      # list-item's own outer fiber instead, which is exactly what makes
      # cross-item coalescing work at all. See execution/dataloader.di's
      # own header comment for why only the list-item granularity is
      # batched, not sibling fields too -- this is why: sibling-field
      # batching would need its own version of the same bubbling-up
      # machinery this file's own header explicitly scoped out as a
      # project-sized undertaking on its own.
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
        # `__schema`/`__type` are meta-fields of the query root specifically
        # (per spec), not available on every type the way `__typename` is --
        # `object_type == @schema.query_type()` is a plain reference-equality
        # check (confirmed directly: Diamond's default Instance `==` with no
        # custom override does identity comparison), true only when this
        # selection is a true top-level query field.
        is_root = object_type == @schema.query_type()
        if is_root && field_name == "__schema"
          schema_field = GraphQL::Field.new("__schema", @schema_meta_type.non_null(), nil)
          resolved_value = @schema
        elsif is_root && field_name == "__type"
          schema_field = GraphQL::Field.new("__type", @type_meta_type, nil, @type_name_argument)
          begin
            args = GraphQL::Execution::Coercion.coerce_arguments(field_node.arguments(), @type_name_argument, coerced_variables)
          rescue e: StandardError
            @errors.push({"message": e.message(), "path": path})
            return nil
          end
          resolved_value = @type_map[args["name"]]
        else
          schema_field = object_type.field_named(field_name)
          if schema_field == nil
            @errors.push({"message": "field \"#{field_name}\" not found on type \"#{object_type.name()}\"", "path": path})
            return nil
          end
          begin
            args = GraphQL::Execution::Coercion.coerce_arguments(field_node.arguments(), schema_field.arguments(), coerced_variables)
            # `context["lookahead"]` is set fresh, in place, immediately
            # before every resolver call -- safe because execution is
            # single-threaded and fully synchronous (each resolver call
            # completes, including everything nested under it, before the
            # next field's own call begins), not because `context` is
            # copied per field. A resolver reading `context["lookahead"]`
            # is expected to do so synchronously, during its own call --
            # stashing `context` away and reading it back later would see
            # whichever field resolved most recently, not its own.
            context["lookahead"] = GraphQL::Execution::Lookahead.new(
              self.merged_selection_set(fields), fragments, coerced_variables)
            resolver = schema_field.resolve()
            resolved_value = if resolver == nil
              if schema_field.arguments().length() != 0
                raise GraphQL::ExecutionError.new(
                  "Field '#{field_name}' declares arguments and needs an explicit resolver")
              end
              object_value.public_send(field_name)
            else
              resolver(object_value, args, context)
            end
          rescue e: StandardError
            @errors.push({"message": e.message(), "path": path})
            if schema_field.type().kind() == "NON_NULL"
              raise NullBubbleError.new(e.message())
            end
            return nil
          end
        end
        begin
          self.complete_value(schema_field.type(), fields, resolved_value, context, coerced_variables, fragments, path)
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
          type.coerce_result()(result)
        end
      end

      # One Fiber per list item -- the classic N+1 shape this whole feature
      # exists for (N parents in a list, each independently resolving the
      # same child field/association), driven together by `@dataloader` so
      # every item's own `.load()` call for that field coalesces into one
      # batch dispatch instead of firing N separate ones. `output` is
      # pre-sized with `nil` placeholders upfront so each fiber can write
      # `output[index] = ...` directly (fibers don't necessarily finish in
      # order, so `#push`ing from inside each one wouldn't preserve
      # position) -- every existing line of resolution/error-handling logic
      # below is unchanged from before this feature, just moved inside a
      # per-item closure instead of a loop body.
      #
      # `closure body()`, not a plain nested `def` -- confirmed directly
      # against `docs/syntax.md`'s own "closure name() ... end" section:
      # a plain nested `def` is "built for exactly one job: a detached
      # patch, meant to be handed to define_method/redefine_method"; called
      # directly instead, its own `self`/`@ivar` references "don't mean
      # anything." `closure` is the documented form for a nested function
      # that's called immediately and needs `self` -- which is exactly this
      # case (`self.complete_value(...)`, `@errors.push(...)` below). Using
      # plain `def` here originally was a real mistake, not a compiler bug
      # -- it read `self` back as `nil` and (separately) broke Callable
      # arity-checking when handed to `Array#map` elsewhere, both symptoms
      # of the same misuse, both go away with `closure`.
      def complete_list_value(type, fields, result, context, coerced_variables, fragments, path)
        unless result is Array
          raise GraphQL::ExecutionError.new("expected a list for \"#{type.name()}\"")
        end
        item_type = type.of_type()
        output = []
        index = 0
        while index < result.length()
          output.push(nil)
          index += 1
        end
        fibers = []
        index = 0
        while index < result.length()
          item_index = index
          item_value = result[index]
          item_path = path.concat([index])
          closure body()
            begin
              output[item_index] = self.complete_value(item_type, fields, item_value, context, coerced_variables, fragments, item_path)
            rescue e: NullBubbleError
              if item_type.kind() == "NON_NULL"
                raise e
              end
              output[item_index] = nil
            rescue e: StandardError
              @errors.push({"message": e.message(), "path": item_path})
              if item_type.kind() == "NON_NULL"
                raise NullBubbleError.new(e.message())
              end
              output[item_index] = nil
            end
          end
          fibers.push(Fiber.new(body))
          index += 1
        end
        @dataloader.run(fibers, context)
        output
      end
    end

  end
end
