# @include/@skip evaluation -- factored out of execution/executor.di so
# execution/lookahead.di can share the exact same logic (a resolver's
# lookahead into its own sub-selections has to honor @include/@skip the
# same way real execution does, or it'd answer a different question
# than "what will actually be in the response").
module GraphQL
  module Execution

    class Directives
      # true/false/nil (nil meaning: this directive wasn't present at all).
      def self.value(directives, directive_name, coerced_variables)
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

      def self.included?(directives, coerced_variables)
        if self.value(directives, "skip", coerced_variables) == true
          return false
        end
        if self.value(directives, "include", coerced_variables) == false
          return false
        end
        true
      end
    end

  end
end
