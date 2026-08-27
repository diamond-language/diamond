class UnionBlockBase
  def transform(value, &block: Callable[[String], String]) -> String
    yield(value)
  end
end

class UnionBlockLeft < UnionBlockBase
end

class UnionBlockRight < UnionBlockBase
end

def transform_union(
  receiver: UnionBlockLeft | UnionBlockRight,
  value: String
) -> String
  receiver.transform(value) do |text|
    text + " shared"
  end
end

puts(transform_union(UnionBlockLeft.new(), "left"))
puts(transform_union(UnionBlockRight.new(), "right"))
