[head, *middle, tail] = [1, 2, 3, 4]
puts([head, middle, tail])
[first, *empty, last] = [5, 6]
puts([first, empty, last])
[outer, [left, *inside, right], final] = [0, [1, 2, 3, 4], 5]
puts([outer, left, inside, right, final])
prefix, *center, suffix = [7, 8, 9, 10]
puts([prefix, center, suffix])
class MiddleHolder
  def unpack(value)
    [@first, *@middle, @last] = value
    [@first, @middle, @last]
  end
end
puts(MiddleHolder.new().unpack([11, 12, 13, 14]))
