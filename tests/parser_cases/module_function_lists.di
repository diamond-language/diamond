module Values
  def first()
    1
  end

  def second()
    2
  end

  module_function(first, second)
end

class Box
  include Values
end

first = begin
  Box.new().first()
rescue error
  20
end

second = begin
  Box.new().second()
rescue error
  22
end

puts(first + second)
