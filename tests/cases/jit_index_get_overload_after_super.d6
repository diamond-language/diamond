class Base
  def initialize(data)
    nil
  end
end

class Box
  def [](key) = key + 100
end

class Container < Base
  attr_accessor result
  def initialize(data: Hash, box)
    super(data)
    @result = box[5]
  end
end

def run()
  total = 0
  index = 0
  while index < 10
    c = Container.new({}, Box.new())
    total = total + c.result()
    index = index + 1
  end
  total
end
run()
