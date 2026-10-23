class Dog
end

class Cat
end

def require_dog(value: Dog | Cat) -> Dog
  copy = value
  if copy is Dog
    return copy
  else
    return Dog.new()
  end
end

require_dog(Cat.new()) is Dog
