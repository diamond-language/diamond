def apply_reference(&block: Callable[[String], String]) -> String
  yield("reference")
end

callable = apply_reference
puts(callable() do |value|
  value + " typed"
end)

def apply_reference_spread(&block: Callable[[Int], Int]) -> Int
  yield(9)
end

spread_callable = apply_reference_spread
puts(spread_callable(*[]) do |value|
  value * 4
end)
