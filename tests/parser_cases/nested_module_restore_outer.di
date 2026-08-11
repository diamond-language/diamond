module Outer
  module Inner
    def inner()
      1
    end
  end

  def answer()
    42
  end

  module_function answer
end

puts(Outer.answer())
