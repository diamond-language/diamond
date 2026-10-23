# The memory budget bounds the whole process: the main program holds about
# 100 KB and the thread allocates about 100 KB, each under the 160,000-byte
# budget on its own, but the thread's allocations push the total over it.
def fill(count)
  items = []
  index = 0
  while index < count
    items.push("x" * 100)
    index += 1
  end
  items
end

def worker(count)
  begin
    fill(count)
    "within"
  rescue error: ResourceLimitError
    "tripped"
  end
end

held = fill(700)
puts(held.length())
puts(Thread.new(worker, 700).join())
