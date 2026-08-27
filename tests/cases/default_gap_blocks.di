def default_gap_function(prefix = "function", &block: Callable[[String], String])
  yield(prefix)
end

puts(default_gap_function() do |value|
  value + " block"
end)

class DefaultGapReceiver
  def transform(prefix = "method", &block: Callable[[String], String])
    yield(prefix)
  end
end

receiver = DefaultGapReceiver.new()
puts(receiver.transform() do |value|
  value + " block"
end)

def invoke_default_gap(
  callback: Callable[[String, Callable[[String], String]], String]
)
  callback() do |value|
    value + " block"
  end
end

def default_gap_target(
  prefix: String = "callable",
  &block: Callable[[String], String]
) -> String
  yield(prefix)
end

puts(invoke_default_gap(default_gap_target))

puts(receiver.transform(*[]) do |value|
  value + " spread block"
end)
