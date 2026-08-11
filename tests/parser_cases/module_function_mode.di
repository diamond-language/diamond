module Values
  module_function

  def answer()
    1
  end
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
