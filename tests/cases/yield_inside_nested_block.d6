# yield inside a do-block calls the enclosing method's &block.
def each_twice(items, &visit)
  items.each() do |x|
    yield(x)
    yield(x * 10)
  end
end
def nested(&blk)
  [1].map() do |a|
    [2].map() do |b| yield(a + b) end
  end
end
def typed(&blk: Callable[[Int], Int]) -> Array
  [1, 2].map() do |x| yield(x) + 1 end
end
def shadowed(&blk)
  [5].map() do |blk| blk end
end
seen = []
each_twice([1, 2]) do |v| seen.push(v) end
[seen, nested() do |v| v * 100 end, typed() do |v| v * 2 end, shadowed() do |v| 0 end]
