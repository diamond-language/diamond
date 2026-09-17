# Array#push/Hash#[]=/instance-variable-write throughput on ordinary,
# never-frozen values -- every one of these now pays a frozen check
# (docs/classes-and-modules.md's freeze/frozen? section) before the
# mutation proceeds, whether or not the receiver is actually frozen. No
# prior bench/*.di coverage existed for freeze/frozen? at all; this is a
# baseline for the common (unfrozen) path's own cost, to compare future
# changes to that check against.
class Box
  def initialize()
    @value = 0
  end
  def value=(v)
    @value = v
  end
end

def run()
  arr = []
  hash = {}
  box = Box.new()
  index = 0
  while index < 200000
    arr.push(index)
    hash[index] = index
    box.value = index
    index = index + 1
  end
  arr.length() + hash.length()
end
run()
