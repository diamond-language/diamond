module Outer
  module Inner
    def value()
      42
    end

    module_function value
  end
end

puts(Outer::Inner.value())
