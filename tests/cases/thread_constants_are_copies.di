ITEMS = [1, 2]

def grow()
  ITEMS.push(3)
  ITEMS.length()
end

inside = Thread.new(grow).join()
[inside, ITEMS.length()].inspect()
