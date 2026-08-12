module Outer
  ANSWER = 42

  module Middle
    module Inner
      def value()
        ANSWER
      end

      module_function value
    end
  end
end

puts(Outer::Middle::Inner.value())
