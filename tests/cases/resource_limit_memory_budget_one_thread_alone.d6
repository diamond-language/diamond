# Control for resource_limit_shared_memory_budget: the same thread, with
# nothing else holding memory, stays under the same budget.
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

puts(Thread.new(worker, 700).join())
