class Box
  attr_accessor value

  def initialize(data: Hash, key)
    @value = data[key]
  end
end

def run()
  key = "x"
  total = 0
  index = 0
  while index < 200
    data = {"x": index}
    box = Box.new(data, key)
    total = total + box.value()
    index = index + 1
  end
  total
end
run()
