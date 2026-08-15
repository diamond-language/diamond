class Err < StandardError
end
def maybe(x: Int) -> Int
  raise Err.new("neg") if x < 0
end
maybe(3)
