class Base
  def initialize(data)
    nil
  end
end

class Swapper < Base
  attr_accessor total
  def initialize(data: Hash, items)
    super(data)
    first = items[0]
    second = items[1]
    items[0] = second
    items[1] = first
    @total = items[0] + items[1] + items[2]
  end
end

def run()
  total = 0
  index = 0
  while index < 50
    s = Swapper.new({}, [1, 2, 3])
    total = total + s.total()
    index = index + 1
  end
  total
end
run()
