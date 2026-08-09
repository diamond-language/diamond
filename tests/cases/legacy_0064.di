class Box
  def initialize(value)
    @value = value
  end

  def value()
    @value
  end
end

def second(values: Array) -> Int
  values[1]
end

nested = [["diamond"], [20, 22], [Box.new("kept alive")]]
second(nested[1])
