module Values
  def answer()
    1
  end

  module_function answer
end

class Box
  include Values
end

value = begin
  Box.new().answer()
rescue error
  42
end

puts(value)
