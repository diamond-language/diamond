class Dog
end

class Cat
end

def require_dog(value: Dog | Cat) -> Dog
  if value is Dog
    return value
  else
    return Dog.new()
  end
end

puts(require_dog(Cat.new()) is Dog)
require_dog(Dog.new()) is Dog
