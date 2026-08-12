module Outer
  module Middle
    module Inner
      def answer()
        42
      end

      module_function answer
    end
  end
end

puts(Outer::Middle::Inner.answer())
