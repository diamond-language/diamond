def invoke_callback(
  callback: Callable[[Callable[[String], String]], String]
) -> String
  callback() do |value|
    value + " contextual"
  end
end

def accept_text(&block: Callable[[String], String]) -> String
  yield("callable")
end

puts(invoke_callback(accept_text))

def invoke_spread(
  callback: Callable[[Callable[[Int], Int]], Int]
) -> Int
  callback(*[]) do |value|
    value * 3
  end
end

def accept_number(&block: Callable[[Int], Int]) -> Int
  yield(7)
end

puts(invoke_spread(accept_number))

def invoke_keyword(
  callback: Callable[[String, Callable[[String], String]], String]
) -> String
  callback(prefix: "keyword") do |value|
    value + " block"
  end
end

def accept_keyword(prefix, &block: Callable[[String], String]) -> String
  yield(prefix)
end

puts(invoke_keyword(accept_keyword))
