module Outer
  ANSWER = 42

  module Inner
    def value()
      ANSWER
    end

    module_function value
  end
end

puts(Outer::Inner.value())
