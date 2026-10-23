class Err < StandardError
end

def foo(x: Int) -> Int
  if x < 0
    raise Err.new("negative")
  else
    5
  end
end
foo(3)
