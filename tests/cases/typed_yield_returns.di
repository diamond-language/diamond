def render(&block: Callable[[], String]) -> String
  yield
end

def calculate(&block: Callable[[], Int]) -> Int
  yield
end

puts(render() do
  "rendered"
end)

puts(calculate() do
  42
end)
