def bar() -> Int
  raise RuntimeError.new("bad")
  "not an int"
end
bar()
