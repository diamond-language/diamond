class Dog
end

class Cat
end

def require_dog(value: Dog | Cat, flag: Bool) -> Dog
  chosen = if flag
    value
  else
    value
  end
  if chosen is Dog
    return chosen
  else
    return Dog.new()
  end
end

require_dog(Cat.new(), true) is Dog
