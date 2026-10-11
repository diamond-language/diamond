class ArrayDefaultBox
  def initialize(items: Array = [])
    @items = items
  end
  def items() -> Array = @items
end

def pack(child: Array, label: String) -> Array
  [child, label, []]
end

def run()
  total = 0
  index = 0
  while index < 100
    child = [index, "kept"]
    packed = pack(child, "outer")
    total += packed[0][0]
    unless packed[0][1] == "kept" && packed[1] == "outer"
      raise RuntimeError.new("array elements lost across allocation")
    end
    packed[2].push(index)
    first = ArrayDefaultBox.new()
    second = ArrayDefaultBox.new()
    first.items().push(index)
    unless second.items().length() == 0
      raise RuntimeError.new("default arrays were shared")
    end
    supplied = ArrayDefaultBox.new(child)
    unless supplied.items()[0] == index
      raise RuntimeError.new("explicit array argument lost")
    end
    index += 1
  end
  total
end

run()
