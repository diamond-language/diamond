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
  rescue
    value
  end
  if selected is Dog
    selected
  else
    Dog.new()
  end
end

require_dog(Cat.new(), true) is Dog
