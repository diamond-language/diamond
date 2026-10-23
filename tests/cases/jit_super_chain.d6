class L0
  attr_accessor v0
  def initialize(data)
    @v0 = 100
  end
end
class L1 < L0
  attr_accessor v1
  def initialize(data: Hash)
    super(data)
    @v1 = 101
  end
end
class L2 < L1
  attr_accessor v2
  def initialize(data: Hash)
    super(data)
    @v2 = 102
  end
end
class L3 < L2
  attr_accessor v3
  def initialize(data: Hash)
    super(data)
    @v3 = 103
  end
end
class L4 < L3
  attr_accessor v4
  def initialize(data: Hash)
    super(data)
    @v4 = 104
  end
end
class L5 < L4
  attr_accessor v5
  def initialize(data: Hash)
    super(data)
    @v5 = 105
  end
end

def run()
  index = 0
  result = 0
  while index < 20
    obj = L5.new({})
    result = obj.v0() + obj.v1() + obj.v2() + obj.v3() + obj.v4() + obj.v5()
    index = index + 1
  end
  result
end
run()
