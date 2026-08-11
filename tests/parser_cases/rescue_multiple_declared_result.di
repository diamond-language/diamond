class Dog
end

class Cat
end

def require_dog(value: Dog | Cat, fail: Bool) -> Dog
  selected = begin
    if fail
      raise "boom"
    else
      value
    end
  rescue error: String
    value
  rescue error: Bool
    value
  end
  if selected is Dog
    selected
  else
    Dog.new()
  end
end

puts(require_dog(Cat.new(), true) is Dog)
