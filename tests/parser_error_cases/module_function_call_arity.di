module Math
  def add(left, right)
    left + right
  end

  module_function add
end

Math.add(1)
