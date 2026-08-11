module Math
  def add(left, right)
    left + right
  end

  module_function add
end

puts(Math.add(20, 22))
