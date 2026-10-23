module BaseValue
  def value()
    1
  end
end

module MiddleValue
  include BaseValue

  def value()
    2
  end
end

module OuterValue
  include MiddleValue
end

class Box
  include OuterValue
end

puts(Box.new().value())
