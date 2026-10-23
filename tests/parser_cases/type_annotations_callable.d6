def apply(callback: Callable[[Int], Int], value: Int) -> Int
  callback(value)
end

def make_incrementer()
  def increment(value: Int) -> Int
    value + 1
  end
  increment
end

apply(make_incrementer(), 41)
