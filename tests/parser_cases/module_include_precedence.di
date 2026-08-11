module FirstValue
  def value()
    1
  end
end

module SecondValue
  def value()
    2
  end
end

module CombinedValue
  include FirstValue
  include SecondValue
end

class Box
  include CombinedValue
end

puts(Box.new().value())
