module BaseValue
  def value()
    1
  end
end

module CombinedValue
  include BaseValue

  def value()
    2
  end
end

class Box
  include CombinedValue
end

puts(Box.new().value())
